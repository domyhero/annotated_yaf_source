/*
  +----------------------------------------------------------------------+
  | Yet Another Framework                                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Xinchen Hui  <laruence@php.net>                              |
  +----------------------------------------------------------------------+
*/

/* $Id: yaf_config.c 327559 2012-09-09 06:05:25Z laruence $ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "main/SAPI.h"
#include "Zend/zend_interfaces.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_alloc.h"
#include "ext/standard/php_string.h"
#include "ext/standard/php_filestat.h"

#include "php_yaf.h"
#include "yaf_namespace.h"
#include "yaf_exception.h"
#include "yaf_config.h"

zend_class_entry *yaf_config_ce;
#ifdef HAVE_SPL
extern PHPAPI zend_class_entry *spl_ce_Countable;
#endif

/* {{{ ARG_INFO
 */
ZEND_BEGIN_ARG_INFO_EX(yaf_config_void_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()
/* }}} */

#include "configs/ini.c"
#include "configs/simple.c"

static zval * yaf_config_ini_zval_persistent(zval *zvalue TSRMLS_DC);
static zval * yaf_config_ini_zval_losable(zval *zvalue TSRMLS_DC);

/* {{{ yaf_config_ini_modified
 *  检查配置文件是否修改，修改的话返回最近修改时间，否则返回0	
 */
static int yaf_config_ini_modified(zval * file, long ctime TSRMLS_DC) {
	zval  n_ctime;
	/* 获取文件上一次状态改变的时间 */
	php_stat(Z_STRVAL_P(file), Z_STRLEN_P(file), 7 /* FS_CTIME */ , &n_ctime TSRMLS_CC);
	if (Z_TYPE(n_ctime) != IS_BOOL && ctime != Z_LVAL(n_ctime)) {
		/* 如果文件有改动则返回最近一次状态改变的时间 */
		return Z_LVAL(n_ctime);
	}
	return 0;
}
/* }}} */

/** {{{ static void yaf_config_cache_dtor(yaf_config_cache **cache)
 * 销毁配置文件的缓存
 */
static void yaf_config_cache_dtor(yaf_config_cache **cache) {
	if (*cache) {
		/* 销毁hash表 */
		zend_hash_destroy((*cache)->data);
		/* 销毁持久变量 */
		pefree((*cache)->data, 1);
		/* 销毁cache */
		pefree(*cache, 1);
	}
}
/* }}} */

/** {{{ static void yaf_config_zval_dtor(zval **value)
 */
/* 销毁持久型zval变量，从里面的实现来看，只能销毁string、array、constant */
static void yaf_config_zval_dtor(zval **value) {
	if (*value) {
		switch(Z_TYPE_PP(value)) {
			case IS_STRING:
			case IS_CONSTANT:
				/* if (Z_STRVAL_P(z)[ Z_STRLEN_P(z) ] != '\0') { zend_err..... */
				CHECK_ZVAL_STRING(*value);
				pefree((*value)->value.str.val, 1);
				pefree(*value, 1);
				break;
			case IS_ARRAY:
			case IS_CONSTANT_ARRAY: {
				/* 销毁hash表，删除持久性内存的变量 */
				zend_hash_destroy((*value)->value.ht);
				pefree((*value)->value.ht, 1);
				pefree(*value, 1);
			}
			break;
		}
	}
}
/* }}} */

/** {{{ static void yaf_config_copy_persistent(HashTable *pdst, HashTable *src TSRMLS_DC)
 *	将普通的HashTable src变成持久型的HashTable pdst
 */
static void yaf_config_copy_persistent(HashTable *pdst, HashTable *src TSRMLS_DC) {
	zval **ppzval;
	char *key;
	uint keylen;
	ulong idx;
	/* 遍历src的hash表中的数据 */
	for(zend_hash_internal_pointer_reset(src);
			zend_hash_has_more_elements(src) == SUCCESS;
			zend_hash_move_forward(src)) {

		if (zend_hash_get_current_key_ex(src, &key, &keylen, &idx, 0, NULL) == HASH_KEY_IS_LONG) {
			/* 当前位置的key为long类型 */
			zval *tmp;
			if (zend_hash_get_current_data(src, (void**)&ppzval) == FAILURE) {
				continue;
			}
			/* 将现在的临时变量转为持久型的 */
			tmp = yaf_config_ini_zval_persistent(*ppzval TSRMLS_CC);
			if (tmp)
			/* 将tmp以键idx加入到pdst这个hash表中 */
			zend_hash_index_update(pdst, idx, (void **)&tmp, sizeof(zval *), NULL);

		} else {
			zval *tmp;
			if (zend_hash_get_current_data(src, (void**)&ppzval) == FAILURE) {
				continue;
			}
			/* 将现在的临时变量转为持久型的 */
			tmp = yaf_config_ini_zval_persistent(*ppzval TSRMLS_CC);
			if (tmp)
			/* 将值加入到pdst这个hash表中 */
			zend_hash_update(pdst, key, keylen, (void **)&tmp, sizeof(zval *), NULL);
		}
	}
}
/* }}} */

/** {{{ static void yaf_config_copy_losable(HashTable *ldst, HashTable *src TSRMLS_DC)
 * 实现普通的复制，没有做持久性等工作
 */
static void yaf_config_copy_losable(HashTable *ldst, HashTable *src TSRMLS_DC) {
	zval **ppzval, *tmp;
	char *key;
	ulong idx;
	uint keylen;
	/* 遍历hash表src */
	for(zend_hash_internal_pointer_reset(src);
			zend_hash_has_more_elements(src) == SUCCESS;
			zend_hash_move_forward(src)) {
		/* 根据当前key的类型来选择操作方式 */
		if (zend_hash_get_current_key_ex(src, &key, &keylen, &idx, 0, NULL) == HASH_KEY_IS_LONG) {
			if (zend_hash_get_current_data(src, (void**)&ppzval) == FAILURE) {
				continue;
			}

			tmp = yaf_config_ini_zval_losable(*ppzval TSRMLS_CC);
			zend_hash_index_update(ldst, idx, (void **)&tmp, sizeof(zval *), NULL);

		} else {
			if (zend_hash_get_current_data(src, (void**)&ppzval) == FAILURE) {
				continue;
			}

			tmp = yaf_config_ini_zval_losable(*ppzval TSRMLS_CC);
			zend_hash_update(ldst, key, keylen, (void **)&tmp, sizeof(zval *), NULL);
		}
	}
}
/* }}} */

/** {{{ static zval * yaf_config_ini_zval_persistent(zval *zvalue TSRMLS_DC)
 */
/* 将一个普通变量转换成持久性变量 */
static zval * yaf_config_ini_zval_persistent(zval *zvalue TSRMLS_DC) {
	/* 申请一块持久型的空间 */
	zval *ret = (zval *)pemalloc(sizeof(zval), 1);
	/* 初始化持久变量ret */
	INIT_PZVAL(ret);
	switch (zvalue->type) {
		case IS_RESOURCE:
		case IS_OBJECT:
		/* 如果传入的变量类型为resource或者object的话就直接返回初始化过的ret */
			break;
		case IS_BOOL:
		case IS_LONG:
		case IS_NULL:
		/* 如果传入的变量类型为boolean、long或者null的话就直接返回初始化过的ret */
			break;
		case IS_CONSTANT:
		case IS_STRING:
		/* 传入的变量类型为constant或者string */
				/* 检验字符串是否标准，最后一个字符是'\0' */
				CHECK_ZVAL_STRING(zvalue);
				/* 将ret的类型置为string */
				Z_TYPE_P(ret) = IS_STRING;
				/* 用zvalue的值和长度生成一个存储着相同信息的持久性内存并赋值给value */
				ret->value.str.val = pestrndup(zvalue->value.str.val, zvalue->value.str.len, 1);
				ret->value.str.len = zvalue->value.str.len;
			break;
		case IS_ARRAY:
		case IS_CONSTANT_ARRAY: {
		/* 传入的变量类型为array或者CONSTANT_ARRAY的话 */	
				/* 初始化两个HashTable,并将zvalue的hash的指针赋给original_ht */
				HashTable *tmp_ht, *original_ht = zvalue->value.ht;
				/* 初始化一个持久性的HashTable的存储并赋值给tmp_ht */
				tmp_ht = (HashTable *)pemalloc(sizeof(HashTable), 1);
				if (!tmp_ht) {
					return NULL;
				}
				/* 将tmp_ht初始化为一个和original_ht拥有相同存储单元的数组 */
				zend_hash_init(tmp_ht, zend_hash_num_elements(original_ht), NULL, (dtor_func_t)yaf_config_zval_dtor, 1);
				/* 进行赋值和持久性的转换 */
				yaf_config_copy_persistent(tmp_ht, original_ht TSRMLS_CC);
				/* ret类型设置为array */
				Z_TYPE_P(ret) = IS_ARRAY;
				/* 将tmp_ht这个持久性的hash表的句柄赋值给ret_value.ht */
				ret->value.ht = tmp_ht;
			}
			break;
	}

	return ret;
}
/* }}} */

/** {{{ static zval * yaf_config_ini_zval_losable(zval *zvalue TSRMLS_DC)
 * 	将 IS_CONSTANT IS_STRING IS_ARRAY IS_CONSTANT_ARRAY等类型的变量做普通复制
 */
static zval * yaf_config_ini_zval_losable(zval *zvalue TSRMLS_DC) {
	/* 初始化变量ret */
	zval *ret;
	MAKE_STD_ZVAL(ret);
	switch (zvalue->type) {
		case IS_RESOURCE:
		case IS_OBJECT:
			break;
		case IS_BOOL:
		case IS_LONG:
		case IS_NULL:
			break;
		case IS_CONSTANT:
		case IS_STRING:
			/* 如果zvalue的类型为constant或者string，检验zvalue字符串是否是正常字符串 */
				CHECK_ZVAL_STRING(zvalue);
				/* 将zvalue的值复制给ret */
				ZVAL_STRINGL(ret, zvalue->value.str.val, zvalue->value.str.len, 1);
			break;
		case IS_ARRAY:
		case IS_CONSTANT_ARRAY: {
			/* 如果zvalue的类型为array或者constant array的话则将ret初始化为一个数组，并且使用yaf_config_copy_losable方法将值复制给ret */
				HashTable *original_ht = zvalue->value.ht;
				array_init(ret);
				yaf_config_copy_losable(Z_ARRVAL_P(ret), original_ht TSRMLS_CC);
			}
			break;
	}

	return ret;
}
/* }}} */

/** {{{ static yaf_config_t * yaf_config_ini_unserialize(yaf_config_t *this_ptr, zval *filename, zval *section TSRMLS_DC)
 *	主要功能是从yaf的配置缓存中读取配置信息，然后利用时间判断配置ini文件是否有过修改
 *	没有修改的话则利用缓存中的配置信息实例化一个yaf_config_ini类的实例并返回	
 */
static yaf_config_t * yaf_config_ini_unserialize(yaf_config_t *this_ptr, zval *filename, zval *section TSRMLS_DC) {
	char *key;
	uint len;
	yaf_config_cache **ppval;

	if (!YAF_G(configs)) {
		return NULL;
	}
	/* 格式化字符串并赋值给key，并返回字符串的长度 */
	len = spprintf(&key, 0, "%s#%s", Z_STRVAL_P(filename), Z_STRVAL_P(section));

	if (zend_hash_find(YAF_G(configs), key, len + 1, (void **)&ppval) == SUCCESS) {
		/* 检查配置文件是否修改,如果修改的话则释放key，并返回NULL */
		if (yaf_config_ini_modified(filename, (*ppval)->ctime TSRMLS_CC)) {
			efree(key);
			return NULL;
		} else {
			/* 没修改的话则初始化一个变量props为数组，并将缓存中的值复制给props */
			zval *props;

			MAKE_STD_ZVAL(props);
			array_init(props);
			yaf_config_copy_losable(Z_ARRVAL_P(props), (*ppval)->data TSRMLS_CC);
			efree(key);
			/* tricky way */
			Z_SET_REFCOUNT_P(props, 0);
			/*
			 * 利用props和section的值来初始化一个yaf_config_ini的对象，最后返回这个类的实例；
			 * 这里实现了配置信息的缓存，在多个类的实例中的共享
			 */
			return yaf_config_ini_instance(this_ptr, props, section TSRMLS_CC);
		}
		efree(key);
	}

	return NULL;
}
/* }}} */

/** {{{ static void yaf_config_ini_serialize(yaf_config_t *this_ptr, zval *filename, zval *section TSRMLS_DC)
 *	如果没有初始化YAF_G(configs)，则初始化它，如果初始化了则将它的HashTable转换为持久常驻内存型
 *	并以文件名加section为key，将cache信息存入YAF_G(configs)
 */
static void yaf_config_ini_serialize(yaf_config_t *this_ptr, zval *filename, zval *section TSRMLS_DC) {
	char *key;
	uint len;
	long ctime;
	zval *configs;
	HashTable *persistent;
	yaf_config_cache *cache;

	if (!YAF_G(configs)) {
		/* 为全局变量configs申请一块HashTable的持久型的内存 */
		YAF_G(configs) = (HashTable *)pemalloc(sizeof(HashTable), 1);
		if (!YAF_G(configs)) {
			return;
		}
		/* 初始化configs为一个拥有8个元素的数组 */
		/* TODO 为啥是8？ */
		zend_hash_init(YAF_G(configs), 8, NULL, (dtor_func_t) yaf_config_cache_dtor, 1);
	}
	/* 为变量cache申请一个类型为yaf_config_cache的持久型内存 */
	cache = (yaf_config_cache *)pemalloc(sizeof(yaf_config_cache), 1);

	if (!cache) {
		return;
	}
	/* 为变量persistent申请一块HashTable类型的持久型内存 */
	persistent = (HashTable *)pemalloc(sizeof(HashTable), 1);
	if (!persistent) {
		return;
	}
	/* 获取对象的属性$_config */
	configs = zend_read_property(yaf_config_ini_ce, this_ptr, ZEND_STRL(YAF_CONFIG_PROPERT_NAME), 1 TSRMLS_CC);
	/* 将persistent初始化为一个和$_configs拥有相同数量成员的数组 */
	zend_hash_init(persistent, zend_hash_num_elements(Z_ARRVAL_P(configs)), NULL, (dtor_func_t) yaf_config_zval_dtor, 1);
	/* 复制$_configs中的每一个元素，并且转化为持久型的元素添加到persistent */
	yaf_config_copy_persistent(persistent, Z_ARRVAL_P(configs) TSRMLS_CC);
	/* 获取配置文件最后修改的时间 */
	ctime = yaf_config_ini_modified(filename, 0 TSRMLS_CC);
	/* 为当前的缓存添加时间标识 */
	cache->ctime = ctime;
	/* 将persistent持久型数组作为cache的值 */
	cache->data  = persistent;
	/* key为filename# */
	len = spprintf(&key, 0, "%s#%s", Z_STRVAL_P(filename), Z_STRVAL_P(section));
	/* 以上面拼装成的key将cache储存到全局configs中 */
	zend_hash_update(YAF_G(configs), key, len + 1, (void **)&cache, sizeof(yaf_config_cache *), NULL);
	/* 释放key的内存 */
	efree(key);
}
/* }}} */

/** {{{ yaf_config_t * yaf_config_instance(yaf_config_t *this_ptr, zval *arg1, zval *arg2 TSRMLS_DC)
 */
yaf_config_t * yaf_config_instance(yaf_config_t *this_ptr, zval *arg1, zval *arg2 TSRMLS_DC) {
	yaf_config_t *instance;

	if (!arg1) {
		return NULL;
	}

	if (Z_TYPE_P(arg1) == IS_STRING) {
		/* 判断argv1字符串值的最后三位是否为'ini' */
		if (strncasecmp(Z_STRVAL_P(arg1) + Z_STRLEN_P(arg1) - 3, "ini", 3) == 0) {
			if (YAF_G(cache_config)) {
				/* 如果已经有配置文件的缓存的话就实例化一个对象，从这里看出，arg1是filename，arg2为section */
				if ((instance = yaf_config_ini_unserialize(this_ptr, arg1, arg2 TSRMLS_CC))) {
					return instance;
				}
			}
			/* 读取配置文件并产生一个yaf_config_ini类的实例化 */
			instance = yaf_config_ini_instance(this_ptr, arg1, arg2 TSRMLS_CC);

			if (!instance) {
				return NULL;
			}

			if (YAF_G(cache_config)) {
				/* 如果已经缓存了配置信息，则更新配置信息 */
				yaf_config_ini_serialize(instance, arg1, arg2 TSRMLS_CC);
			}

			return instance;
		}
	}

	if (Z_TYPE_P(arg1) == IS_ARRAY) {
		/* 如果filename接收到的为一个数组，则按照yaf_config_simple的方式来处理配置信息 */
		zval *readonly;

		MAKE_STD_ZVAL(readonly);
		ZVAL_BOOL(readonly, 1);
		instance = yaf_config_simple_instance(this_ptr, arg1, readonly TSRMLS_CC);
		efree(readonly);
		return instance;
	}

	yaf_trigger_error(YAF_ERR_TYPE_ERROR TSRMLS_CC, "Expects a string or an array as parameter");
	return NULL;
}
/* }}} */

/** {{{ yaf_config_methods
*/
zend_function_entry yaf_config_methods[] = {
	PHP_ABSTRACT_ME(yaf_config, get, NULL)
	PHP_ABSTRACT_ME(yaf_config, set, NULL)
	PHP_ABSTRACT_ME(yaf_config, readonly, NULL)
	PHP_ABSTRACT_ME(yaf_config, toArray, NULL)
	{NULL, NULL, NULL}
};
/* }}} */

/** {{{ YAF_STARTUP_FUNCTION
*/
YAF_STARTUP_FUNCTION(config) {
	zend_class_entry ce;

	YAF_INIT_CLASS_ENTRY(ce, "Yaf_Config_Abstract", "Yaf\\Config_Abstract", yaf_config_methods);
	yaf_config_ce = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	/* abstract class Yaf_Config_Abstract */
	yaf_config_ce->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS;
	/* protected $_config=null */
	zend_declare_property_null(yaf_config_ce, ZEND_STRL(YAF_CONFIG_PROPERT_NAME), ZEND_ACC_PROTECTED TSRMLS_CC);
	/* protected $_readonly=true */
	zend_declare_property_bool(yaf_config_ce, ZEND_STRL(YAF_CONFIG_PROPERT_NAME_READONLY), 1, ZEND_ACC_PROTECTED TSRMLS_CC);

	YAF_STARTUP(config_ini);
	YAF_STARTUP(config_simple);

	return SUCCESS;
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
