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

/* $Id: ini.c 327626 2012-09-13 02:57:39Z laruence $ */

zend_class_entry *yaf_config_ini_ce;

yaf_config_t * yaf_config_ini_instance(yaf_config_t *this_ptr, zval *filename, zval *section TSRMLS_DC);

#define YAF_CONFIG_INI_PARSING_START   0
#define YAF_CONFIG_INI_PARSING_PROCESS 1
#define YAF_CONFIG_INI_PARSING_END     2

/** {{{ ARG_INFO
 */
ZEND_BEGIN_ARG_INFO_EX(yaf_config_ini_construct_arginfo, 0, 0, 1)
	ZEND_ARG_INFO(0, config_file)
	ZEND_ARG_INFO(0, section)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(yaf_config_ini_get_arginfo, 0, 0, 0)
	ZEND_ARG_INFO(0, name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(yaf_config_ini_rget_arginfo, 0, 0, 1)
	ZEND_ARG_INFO(0, name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(yaf_config_ini_unset_arginfo, 0, 0, 1)
	ZEND_ARG_INFO(0, name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(yaf_config_ini_set_arginfo, 0, 0, 2)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(yaf_config_ini_isset_arginfo, 0, 0, 1)
	ZEND_ARG_INFO(0, name)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ static void yaf_config_ini_zval_deep_copy(zval **p) 
 * 递归复制产生一个新的数组
 */
static void yaf_config_ini_zval_deep_copy(zval **p) {
	zval *value;
	ALLOC_ZVAL(value);
	*value = **p;

	switch (Z_TYPE_PP(p)) {
		case IS_ARRAY:
			{
				array_init(value);
				zend_hash_copy(Z_ARRVAL_P(value), Z_ARRVAL_PP(p), 
						(copy_ctor_func_t)yaf_config_ini_zval_deep_copy, NULL, sizeof(zval *));
			}
			break;
		default:
			zval_copy_ctor(value);
			Z_TYPE_P(value) = Z_TYPE_PP(p);
	}

	INIT_PZVAL(value);
	*p = value;
}
/* }}} */

/** {{{ zval * yaf_config_ini_format(yaf_config_t *instance, zval **ppzval TSRMLS_DC)
 *  通过调用yaf_config_ini_instance实例化类，并返回类的对象
*/
zval * yaf_config_ini_format(yaf_config_t *instance, zval **ppzval TSRMLS_DC) {
	zval *readonly, *ret;
	readonly = zend_read_property(yaf_config_ini_ce, instance, ZEND_STRL(YAF_CONFIG_PROPERT_NAME_READONLY), 1 TSRMLS_CC);
	ret = yaf_config_ini_instance(NULL, *ppzval, NULL TSRMLS_CC);
	return ret;
}
/* }}} */

/* PHP版本高于5.2 */
#if ((PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION > 2))
/** {{{ static void yaf_config_ini_simple_parser_cb(zval *key, zval *value, zval *index, int callback_type, zval *arr TSRMLS_DC)
*/
/*
 *  INI entries
 *	define ZEND_INI_PARSER_ENTRY     	1 		Normal entry: foo = bar
 *	define ZEND_INI_PARSER_SECTION   	2 		Section: [foobar]
 *	define ZEND_INI_PARSER_POP_ENTRY 	3 		Offset entry: foo[] = bar
 * 
 *	总的来说，这个函数的功能就是传入key和value，然后类型，然后将key分割并把value存储到arr数组中，反正过程挺麻烦的，得结合实际的应用代码进一步分析
 */
static void yaf_config_ini_simple_parser_cb(zval *key, zval *value, zval *index, int callback_type, zval *arr TSRMLS_DC) {
	zval *element;
	switch (callback_type) {
		case ZEND_INI_PARSER_ENTRY:
			{
				/** 
				 *	这一步是对类似param=1或者param.p.z=2这种类型的配置信息进行分割并以数组的形式保存到arr的过程   
				 *	主要步骤是首先在126行对于key进行第一次的以.分割第一节，然后利用循环分割处理key，再通过判断当前key时候已经存在于arr
				 *  的第一维或者多维数组中。
				 *  如果不存在：并且当前key还能分割的话，则生成一个空数组，并已当前分割得到的key组成键值对保存到arr的相应位置。如果当前key不能继续分割，则以当前分割得到的字符串为key，将传进来的value保存到数组arr中
				 *  如果存在，并且对应的值不为数组：如果当前的key还能分割，则初始化一个空数组为value，以分割出的字符串为key，存在数组；不能分割的话则，直接以当前分割出的字符串为key，将value保存到arr数组中
				 */
				char *skey, *seg, *ptr;
				zval **ppzval, *dst;

				if (!value) {
					break;
				}

				dst = arr;
				/* 二进制安全，复制一份key到skey */
				skey = estrndup(Z_STRVAL_P(key), Z_STRLEN_P(key));
				/* 对skey用.进行一部分一部分的分割，分割方式见php内置函数：strtok */
				if ((seg = php_strtok_r(skey, ".", &ptr))) {
					/* 进行循环处理 */
					do {
					    char *real_key = seg;
					    /* 再用.继续分割skey */
						seg = php_strtok_r(NULL, ".", &ptr);
						if (zend_symtable_find(Z_ARRVAL_P(dst), real_key, strlen(real_key) + 1, (void **) &ppzval) == FAILURE) {
							/* 从HashTable dst中用real_key进行查找失败 */
							if (seg) {
								/* 本次循环内的skey分割有效的话，则初始化一个数组tmp，并将它以键real_key添加到数组dst中 */
								zval *tmp;
							    MAKE_STD_ZVAL(tmp);   
								array_init(tmp);
								zend_symtable_update(Z_ARRVAL_P(dst), 
										real_key, strlen(real_key) + 1, (void **)&tmp, sizeof(zval *), (void **)&ppzval);
							} else {
								/* 本次循环内skey分割无效的话，将value的值复制给element，并将它以建real_key添加到数组dst中 */
							    MAKE_STD_ZVAL(element);
								ZVAL_ZVAL(element, value, 1, 0);
								zend_symtable_update(Z_ARRVAL_P(dst), 
										real_key, strlen(real_key) + 1, (void **)&element, sizeof(zval *), NULL);
								break;
							}
						} else {
							/* 从HashTable dst中用real_key进行查找成功 */
							if (IS_ARRAY != Z_TYPE_PP(ppzval)) {
								/* 查找到的值ppzval不是数组 */
								if (seg) {
									/* 本次循环内的skey分割有效的话，则初始化一个数组tmp，并将它以键real_key添加到数组dst中 */
									zval *tmp;
									MAKE_STD_ZVAL(tmp);   
									array_init(tmp);
									zend_symtable_update(Z_ARRVAL_P(dst), 
											real_key, strlen(real_key) + 1, (void **)&tmp, sizeof(zval *), (void **)&ppzval);
								} else {
									/* 本次循环内skey分割无效的话，将value的值复制给element，并将它以建real_key添加到数组dst中 */
									MAKE_STD_ZVAL(element);
									ZVAL_ZVAL(element, value, 1, 0);
									zend_symtable_update(Z_ARRVAL_P(dst), 
											real_key, strlen(real_key) + 1, (void **)&element, sizeof(zval *), NULL);
								}
							} 
						}
						dst = *ppzval;
					} while (seg);
				}
				efree(skey);
			}
			break;

		case ZEND_INI_PARSER_POP_ENTRY:
			{
				zval *hash, **find_hash, *dst;

				if (!value) {
					break;
				}

				if (!(Z_STRLEN_P(key) > 1 && Z_STRVAL_P(key)[0] == '0')
						&& is_numeric_string(Z_STRVAL_P(key), Z_STRLEN_P(key), NULL, NULL, 0) == IS_LONG) {
					/* 传过来的key的长度大于1，并且第一位不能是0的数字型的字符串 */
					ulong skey = (ulong)zend_atol(Z_STRVAL_P(key), Z_STRLEN_P(key));	/* 将字符串形式的key转换成ulong */
					if (zend_hash_index_find(Z_ARRVAL_P(arr), skey, (void **) &find_hash) == FAILURE) {
						/* 从arr重查找上面生成的数字型的key，如果查找失败则初始化一个空数组，并以skey为它的key添加到数组arr中 */
						MAKE_STD_ZVAL(hash);
						array_init(hash);
						zend_hash_index_update(Z_ARRVAL_P(arr), skey, &hash, sizeof(zval *), NULL);
					} else {
						/* 查找成功，则将skey对应的值的地址赋给hash */
						hash = *find_hash;
					}
				} else {
					char *seg, *ptr;
					char *skey = estrndup(Z_STRVAL_P(key), Z_STRLEN_P(key));	/* 二进制安全复制key */

					dst = arr;
					if ((seg = php_strtok_r(skey, ".", &ptr))) {	/* 进行第一次分割 */
						/* 第一次分割成功 */
						while (seg) {
							if (zend_symtable_find(Z_ARRVAL_P(dst), seg, strlen(seg) + 1, (void **) &find_hash) == FAILURE) {
								/* 从arr重查找失败，生成一个空数组，并以分割出的seg为key将空数组存进dst中 */
								MAKE_STD_ZVAL(hash);
								array_init(hash);
								zend_symtable_update(Z_ARRVAL_P(dst), 
										seg, strlen(seg) + 1, (void **)&hash, sizeof(zval *), (void **)&find_hash);
							}
							dst = *find_hash;	/* 深入到查找成功或者上面生成的空数组中去 */
							seg = php_strtok_r(NULL, ".", &ptr);	/* 继续分割传进来的key */
						}
						hash = dst;
					} else {
						/* 第一次分割失败 */
						if (zend_symtable_find(Z_ARRVAL_P(dst), seg, strlen(seg) + 1, (void **)&find_hash) == FAILURE) {
							/* 查找失败则生成一个空数组，以seg为key存进数组dst中 */
							MAKE_STD_ZVAL(hash);
							array_init(hash);
							zend_symtable_update(Z_ARRVAL_P(dst), seg, strlen(seg) + 1, (void **)&hash, sizeof(zval *), NULL);
						} else {
							/* 查找成功 */
							hash = *find_hash;
						}
					}
					efree(skey);
				}

				if (Z_TYPE_P(hash) != IS_ARRAY) {
					/* 经过上面过程产生的hash不为数组的话，则摧毁原来的类型结构，生成一个新的空数组 */
					zval_dtor(hash);
					INIT_PZVAL(hash);
					array_init(hash);
				}

				MAKE_STD_ZVAL(element);
				ZVAL_ZVAL(element, value, 1, 0);

				if (index && Z_STRLEN_P(index) > 0) {	/* 字符串key处理 */
					add_assoc_zval_ex(hash, Z_STRVAL_P(index), Z_STRLEN_P(index) + 1, element);
				} else {	/* 数字key处理 */
					add_next_index_zval(hash, element);
				}
			}
			break;

		case ZEND_INI_PARSER_SECTION:
			break;
	}
}
/* }}} */

/** {{{ static void yaf_config_ini_parser_cb(zval *key, zval *value, zval *index, int callback_type, zval *arr TSRMLS_DC)
*/
static void yaf_config_ini_parser_cb(zval *key, zval *value, zval *index, int callback_type, zval *arr TSRMLS_DC) {
	/* 读取yaf的全局变量获取文件解析的进度标识，如果已经结束则直接返回空 */
	if (YAF_G(parsing_flag) == YAF_CONFIG_INI_PARSING_END) {
		return;
	}
	/* 解析标识:Section: [foobar] */
	if (callback_type == ZEND_INI_PARSER_SECTION) {
		zval **parent;
		char *seg, *skey;
		uint skey_len;
		/* 解析进行中 */
		if (YAF_G(parsing_flag) == YAF_CONFIG_INI_PARSING_PROCESS) {
			/* 置为解析结束，返回空值 */
			YAF_G(parsing_flag) = YAF_CONFIG_INI_PARSING_END;
			return;
		}
		/* 将key复制产生一个skey */
		skey = estrndup(Z_STRVAL_P(key), Z_STRLEN_P(key));
		/* 初始化全局变量active_ini_file_section为一个数组 */
		MAKE_STD_ZVAL(YAF_G(active_ini_file_section));
		array_init(YAF_G(active_ini_file_section));
		/* 从skey重查找第一次出现：的位置的指针 */
		if ((seg = strchr(skey, ':'))) {
			/* 查找成功、、、 */
			char *section;

			while (*(seg) == ' ' || *(seg) == ':') {
				*(seg++) = '\0';	/* 将空格或者：换成字符串结束符，并将指针后移 */
			}
			/* 在查找在seg中最后出现：的位置的指针 */
			if ((section = strrchr(seg, ':'))) {
			    /* muilt-inherit */
				do {
					while (*(section) == ' ' || *(section) == ':') {
						*(section++) = '\0';	/* 将空格或者：换成字符串结束符，并将指针后移 */
					}
					if (zend_symtable_find(Z_ARRVAL_P(arr), section, strlen(section) + 1, (void **)&parent) == SUCCESS) {
						/* arr中查找section成功，则将查找到的数组值复制到YAF_G(active_ini_file_section) */
						zend_hash_copy(Z_ARRVAL_P(YAF_G(active_ini_file_section)), Z_ARRVAL_PP(parent),
							   	(copy_ctor_func_t)yaf_config_ini_zval_deep_copy, NULL, sizeof(zval *));
					}
				} while ((section = strrchr(seg, ':')));	/* 继续查找在seg中最后出现：的位置的指针 */
			}

			/* remove the tail space, thinking of 'foo : bar : test' */
            section = seg + strlen(seg) - 1;	/* 移动字符串seg指针到最后一个有效字符 */
			while (*section == ' ' || *section == ':') {
				*(section--) = '\0';	/* 去除最后的那个空格或者：，让seg成为一个合格的字符串 */
			}
			/* 又是查找复制的过程 */
			if (zend_symtable_find(Z_ARRVAL_P(arr), seg, strlen(seg) + 1, (void **)&parent) == SUCCESS) {
				zend_hash_copy(Z_ARRVAL_P(YAF_G(active_ini_file_section)), Z_ARRVAL_PP(parent),
						(copy_ctor_func_t)yaf_config_ini_zval_deep_copy, NULL, sizeof(zval *));
			}
		}
	    seg = skey + strlen(skey) - 1;	/* 将skey字符串的指针移到最后一位 */
        while (*seg == ' ' || *seg == ':') {
			*(seg--) = '\0';	/* 如果最后一位是空格或者：，则替换成字符串结束符 */
		}
		/* 将数组YAF_G(active_ini_file_section)的值以skey为键更新到arr中 */
		skey_len = strlen(skey);
		zend_symtable_update(Z_ARRVAL_P(arr), skey, skey_len + 1, &YAF_G(active_ini_file_section), sizeof(zval *), NULL);
		if (YAF_G(ini_wanted_section) && Z_STRLEN_P(YAF_G(ini_wanted_section)) == skey_len
				&& !strncasecmp(Z_STRVAL_P(YAF_G(ini_wanted_section)), skey, skey_len)) {
			YAF_G(parsing_flag) = YAF_CONFIG_INI_PARSING_PROCESS;
		}
		efree(skey);
	} else if (value) {
		zval *active_arr;
		if (YAF_G(active_ini_file_section)) {
			active_arr = YAF_G(active_ini_file_section);
		} else {
			active_arr = arr;
		}
		yaf_config_ini_simple_parser_cb(key, value, index, callback_type, active_arr TSRMLS_CC);
	}
}
/* }}} */
#else 
/** {{{ static void yaf_config_ini_simple_parser_cb(zval *key, zval *value, int callback_type, zval *arr)
 * 这里是为PHP版本低于或者等于5.2的做的兼容处理，整体过程跟上面差不多，就不做分析
*/
static void yaf_config_ini_simple_parser_cb(zval *key, zval *value, int callback_type, zval *arr) {
	zval *element;
	switch (callback_type) {
		case ZEND_INI_PARSER_ENTRY:
			{
				char *skey, *seg, *ptr;
				zval **ppzval, *dst;

				if (!value) {
					break;
				}

				dst = arr;
				skey = estrndup(Z_STRVAL_P(key), Z_STRLEN_P(key));
				if ((seg = php_strtok_r(skey, ".", &ptr))) {
					do {
					    char *real_key = seg;
						seg = php_strtok_r(NULL, ".", &ptr);
						if (zend_symtable_find(Z_ARRVAL_P(dst), real_key, strlen(real_key) + 1, (void **) &ppzval) == FAILURE) {
							if (seg) {
								zval *tmp;
							    MAKE_STD_ZVAL(tmp);   
								array_init(tmp);
								zend_symtable_update(Z_ARRVAL_P(dst), 
										real_key, strlen(real_key) + 1, (void **)&tmp, sizeof(zval *), (void **)&ppzval);
							} else {
							    MAKE_STD_ZVAL(element);
								ZVAL_ZVAL(element, value, 1, 0);
								zend_symtable_update(Z_ARRVAL_P(dst), 
										real_key, strlen(real_key) + 1, (void **)&element, sizeof(zval *), NULL);
								break;
							}
						} else {
							if (IS_ARRAY != Z_TYPE_PP(ppzval)) {
								if (seg) {
									zval *tmp;
									MAKE_STD_ZVAL(tmp);   
									array_init(tmp);
									zend_symtable_update(Z_ARRVAL_P(dst), 
											real_key, strlen(real_key) + 1, (void **)&tmp, sizeof(zval *), (void **)&ppzval);
								} else {
									MAKE_STD_ZVAL(element);
									ZVAL_ZVAL(element, value, 1, 0);
									zend_symtable_update(Z_ARRVAL_P(dst), 
											real_key, strlen(real_key) + 1, (void **)&element, sizeof(zval *), NULL);
								}
							} 
						}
						dst = *ppzval;
					} while (seg);
				}
				efree(skey);
			}
			break;

		case ZEND_INI_PARSER_POP_ENTRY:
			{
				zval *hash, **find_hash, *dst;

				if (!value) {
					break;
				}

				if (!(Z_STRLEN_P(key) > 1 && Z_STRVAL_P(key)[0] == '0')
						&& is_numeric_string(Z_STRVAL_P(key), Z_STRLEN_P(key), NULL, NULL, 0) == IS_LONG) {
					ulong skey = (ulong)zend_atol(Z_STRVAL_P(key), Z_STRLEN_P(key));
					if (zend_hash_index_find(Z_ARRVAL_P(arr), skey, (void **) &find_hash) == FAILURE) {
						MAKE_STD_ZVAL(hash);
						array_init(hash);
						zend_hash_index_update(Z_ARRVAL_P(arr), skey, &hash, sizeof(zval *), NULL);
					} else {
						hash = *find_hash;
					}
				} else {
					char *seg, *ptr;
					char *skey = estrndup(Z_STRVAL_P(key), Z_STRLEN_P(key));

					dst = arr;
					if ((seg = php_strtok_r(skey, ".", &ptr))) {
						while (seg) {
							if (zend_symtable_find(Z_ARRVAL_P(dst), seg, strlen(seg) + 1, (void **) &find_hash) == FAILURE) {
								MAKE_STD_ZVAL(hash);
								array_init(hash);
								zend_symtable_update(Z_ARRVAL_P(dst), 
										seg, strlen(seg) + 1, (void **)&hash, sizeof(zval *), (void **)&find_hash);
							}
							dst = *find_hash;
							seg = php_strtok_r(NULL, ".", &ptr);
						}
						hash = dst;
					} else {
						if (zend_symtable_find(Z_ARRVAL_P(dst), seg, strlen(seg) + 1, (void **)&find_hash) == FAILURE) {
							MAKE_STD_ZVAL(hash);
							array_init(hash);
							zend_symtable_update(Z_ARRVAL_P(dst), seg, strlen(seg) + 1, (void **)&hash, sizeof(zval *), NULL);
						} else {
							hash = *find_hash;
						}
					}
					efree(skey);
				}

				if (Z_TYPE_P(hash) != IS_ARRAY) {
					zval_dtor(hash);
					INIT_PZVAL(hash);
					array_init(hash);
				}

				MAKE_STD_ZVAL(element);
				ZVAL_ZVAL(element, value, 1, 0);
				add_next_index_zval(hash, element);
			}
			break;

		case ZEND_INI_PARSER_SECTION:
			break;
	}
}
/* }}} */

/** {{{ static void yaf_config_ini_parser_cb(zval *key, zval *value, int callback_type, zval *arr)
*/
static void yaf_config_ini_parser_cb(zval *key, zval *value, int callback_type, zval *arr) {
	TSRMLS_FETCH();

	if (YAF_G(parsing_flag) == YAF_CONFIG_INI_PARSING_END) {
		return;
	}

	if (callback_type == ZEND_INI_PARSER_SECTION) {
		zval **parent;
		char *seg, *skey;
		uint skey_len;

		if (YAF_G(parsing_flag) == YAF_CONFIG_INI_PARSING_PROCESS) {
			YAF_G(parsing_flag) = YAF_CONFIG_INI_PARSING_END;
			return;
		}

		skey = estrndup(Z_STRVAL_P(key), Z_STRLEN_P(key));

		MAKE_STD_ZVAL(YAF_G(active_ini_file_section));
		array_init(YAF_G(active_ini_file_section));

		if ((seg = strchr(skey, ':'))) {
			char *section;

			while (*(seg) == ' ' || *(seg) == ':') {
				*(seg++) = '\0';
			}

			if ((section = strrchr(seg, ':'))) {
			    /* muilt-inherit */
				do {
					while (*(section) == ' ' || *(section) == ':') {
						*(section++) = '\0';
					}
					if (zend_symtable_find(Z_ARRVAL_P(arr), section, strlen(section) + 1, (void **)&parent) == SUCCESS) {
						zend_hash_copy(Z_ARRVAL_P(YAF_G(active_ini_file_section)), Z_ARRVAL_PP(parent),
							   	(copy_ctor_func_t)yaf_config_ini_zval_deep_copy, NULL, sizeof(zval *));
					}
				} while ((section = strrchr(seg, ':')));
			}

			/* remove the tail space, thinking of 'foo : bar : test' */
            section = seg + strlen(seg) - 1;
			while (*section == ' ' || *section == ':') {
				*(section--) = '\0';
			}

			if (zend_symtable_find(Z_ARRVAL_P(arr), seg, strlen(seg) + 1, (void **)&parent) == SUCCESS) {
				zend_hash_copy(Z_ARRVAL_P(YAF_G(active_ini_file_section)), Z_ARRVAL_PP(parent),
						(copy_ctor_func_t)yaf_config_ini_zval_deep_copy, NULL, sizeof(zval *));
			}
		}
	    seg = skey + strlen(skey) - 1;
        while (*seg == ' ' || *seg == ':') {
			*(seg--) = '\0';
		}	
		skey_len = strlen(skey);
		zend_symtable_update(Z_ARRVAL_P(arr), skey, skey_len + 1, &YAF_G(active_ini_file_section), sizeof(zval *), NULL);
		if (YAF_G(ini_wanted_section) && Z_STRLEN_P(YAF_G(ini_wanted_section)) == skey_len
				&& !strncasecmp(Z_STRVAL_P(YAF_G(ini_wanted_section)), skey, Z_STRLEN_P(YAF_G(ini_wanted_section)))) {
			YAF_G(parsing_flag) = YAF_CONFIG_INI_PARSING_PROCESS;
		}
		efree(skey);
	} else if (value) {
		zval *active_arr;
		if (YAF_G(active_ini_file_section)) {
			active_arr = YAF_G(active_ini_file_section);
		} else {
			active_arr = arr;
		}
		yaf_config_ini_simple_parser_cb(key, value, callback_type, active_arr);
	}
}
/* }}} */
#endif

/** {{{ yaf_config_t * yaf_config_ini_instance(yaf_config_t *this_ptr, zval *filename, zval *section_name TSRMLS_DC)
 *  根据传入的filename的类型来进行类的类的初始化的方式，如果传入的是真正的文件名的话，就读取配置文件的信息，
 *	然后实例化类，并返回对象
 */
yaf_config_t * yaf_config_ini_instance(yaf_config_t *this_ptr, zval *filename, zval *section_name TSRMLS_DC) {
	yaf_config_t *instance;
	zval *configs = NULL;

	if (filename && Z_TYPE_P(filename) == IS_ARRAY) {
		/* filename存在，并且值类型为一个数组 */

		if (this_ptr) {
			instance = this_ptr;
		} else {
			/* 初始化一个Yaf_Config_Ini类的对象 */
			MAKE_STD_ZVAL(instance);
			object_init_ex(instance, yaf_config_ini_ce);
		}
		/* 将前面生成或者传递进来的对象赋值给$_config */
		zend_update_property(yaf_config_ini_ce, instance, ZEND_STRL(YAF_CONFIG_PROPERT_NAME), filename TSRMLS_CC);
		/* 返回对象 */
		return instance;
	} else if (filename && Z_TYPE_P(filename) == IS_STRING) {
		/* filename存在，并且值类型为一个字符串 */

	    struct stat sb;
	    /*
		typedef struct _zend_file_handle {
			zend_stream_type  type;
			const char        *filename;
			char              *opened_path;
			union {
				int           fd;
				FILE          *fp;
				zend_stream   stream;
			} handle;
			zend_bool free_filename;
		} zend_file_handle;
	    */
	    zend_file_handle fh = {0};
	    /*将filename的值赋给ini_file*/
		char *ini_file = Z_STRVAL_P(filename);
		/* 初始化configs，并赋值为null */
		MAKE_STD_ZVAL(configs);
		ZVAL_NULL(configs);
		/* 获取文件信息，保存在sb所指的结构体stat中 */
		if (VCWD_STAT(ini_file, &sb) == 0) {
			/* 判断是否为一般文件 */
			if (S_ISREG(sb.st_mode)) {
				/* VCWD_FOPEN => fopen() */
				if ((fh.handle.fp = VCWD_FOPEN(ini_file, "r"))) {
					fh.filename = ini_file;
					/* TODO */
					fh.type = ZEND_HANDLE_FP;
					YAF_G(active_ini_file_section) = NULL;
					/* ini文件解析标致 */
					YAF_G(parsing_flag) = YAF_CONFIG_INI_PARSING_START;
					/* 设置ini文件解析的section名称 */
					if (section_name && Z_STRLEN_P(section_name)) {
						YAF_G(ini_wanted_section) = section_name;
					} else {
						YAF_G(ini_wanted_section) = NULL;
					}
					/* 初始化需要存配置文件信息的数组 */
	 				array_init(configs);
	 				/* 根据php版本设置解析ini文件的api */
#if ((PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION > 2))
					if (zend_parse_ini_file(&fh, 0, 0 /* ZEND_INI_SCANNER_NORMAL */,
						   	(zend_ini_parser_cb_t)yaf_config_ini_parser_cb, configs TSRMLS_CC) == FAILURE
							|| Z_TYPE_P(configs) != IS_ARRAY)
#else
					if (zend_parse_ini_file(&fh, 0, (zend_ini_parser_cb_t)yaf_config_ini_parser_cb, configs) == FAILURE
							|| Z_TYPE_P(configs) != IS_ARRAY)
#endif
					{
						zval_ptr_dtor(&configs);
						yaf_trigger_error(E_ERROR TSRMLS_CC, "Parsing ini file '%s' failed", ini_file);
						return NULL;
					}
				}
			} else {
				zval_ptr_dtor(&configs);
				yaf_trigger_error(E_ERROR TSRMLS_CC, "Argument is not a valid ini file '%s'", ini_file);
				return NULL;
			}
		} else {
			zval_ptr_dtor(&configs);
			yaf_trigger_error(E_ERROR TSRMLS_CC, "Unable to find config file '%s'", ini_file);
			return NULL;
		}

		if (section_name && Z_STRLEN_P(section_name)) {
			/* 如果输出了section_name */ 
			zval **section;
			zval tmp;
			/* 从configs这个存储着所有配置文件的数组中找出setction所代表的名称的节，并将这部分数据赋值给section */
			if (zend_symtable_find(Z_ARRVAL_P(configs),
						Z_STRVAL_P(section_name), Z_STRLEN_P(section_name) + 1, (void **)&section) == FAILURE) {
				zval_ptr_dtor(&configs);
				yaf_trigger_error(E_ERROR TSRMLS_CC, "There is no section '%s' in '%s'", Z_STRVAL_P(section_name), ini_file);
				return NULL;
			}
			/* 将section中的值复制一份到tmp */
			INIT_PZVAL(&tmp);
			array_init(&tmp);
			zend_hash_copy(Z_ARRVAL(tmp), Z_ARRVAL_PP(section), (copy_ctor_func_t) zval_add_ref, NULL, sizeof(zval *));
			zval_dtor(configs);
			/* 再将复制得到的tmp赋值给configs */
			*configs = tmp;
		} 

		/** 
		 *  此处是单例的实现，如果赋予了对象则在后面直接返回传递进来的对象
		 *  如果没有传递对象进来的话，则初始化一个yaf_config_ini_ce对象的实例并且在后面返回
		 */
		if (this_ptr) {
			instance = this_ptr;
		} else {
			MAKE_STD_ZVAL(instance);
			object_init_ex(instance, yaf_config_ini_ce);
		}
		/* 将需要的配置文件的内容赋给类属性$_config */
		zend_update_property(yaf_config_ini_ce, instance, ZEND_STRL(YAF_CONFIG_PROPERT_NAME), configs TSRMLS_CC);
		zval_ptr_dtor(&configs);
		/* 返回类本身$this */
		return instance;
	} else {
		yaf_trigger_error(YAF_ERR_TYPE_ERROR TSRMLS_CC, "Invalid parameters provided, must be path of ini file");
		return NULL;
	}
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::__construct(mixed $config_path, string $section_name)
*/
PHP_METHOD(yaf_config_ini, __construct) {
	zval *filename, *section = NULL;
	/* 获取配置文件名和配置文件节名 */
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|z", &filename, &section) == FAILURE) {
		/* 申明一个变量指针变量prop，并将它初始化为一个数组 */
		zval *prop;
		MAKE_STD_ZVAL(prop);
		array_init(prop);
		
		/* $_config = array(); */
		zend_update_property(yaf_config_ini_ce, getThis(), ZEND_STRL(YAF_CONFIG_PROPERT_NAME), prop TSRMLS_CC);
		zval_ptr_dtor(&prop);
		return;
	}
	/* 进行一系列解析ini文件和实例化的过程 */
	(void)yaf_config_ini_instance(getThis(), filename, section TSRMLS_CC);
}
/** }}} */

/** {{{ proto public Yaf_Config_Ini::get(string $name = NULL)
*/
PHP_METHOD(yaf_config_ini, get) {
	zval *ret, **ppzval;
	char *name;
	uint len = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s", &name, &len) == FAILURE) {
		return;
	}

	if (!len) {	
		/* 如果没有输入任何参数，则直接返回对象本身 */
		RETURN_ZVAL(getThis(), 1, 0);
	} else {
		zval *properties;
		char *entry, *seg, *pptr;

		properties = zend_read_property(yaf_config_ini_ce, getThis(), ZEND_STRL(YAF_CONFIG_PROPERT_NAME), 1 TSRMLS_CC);	/*读取$_config*/

		if (Z_TYPE_P(properties) != IS_ARRAY) {
			/* 如果$_configs不是数组，则直接返回NULL */
			RETURN_NULL();
		}

		entry = estrndup(name, len);	/* 复制name */
		/* 对name='database'或者name='database.param.host'形式的输入进行查找 */
		if ((seg = php_strtok_r(entry, ".", &pptr))) {
			/* name支持原生的格式，即database.params.host这种形式，下面就是执行循环切割查找 */
			while (seg) {
				if (zend_hash_find(Z_ARRVAL_P(properties), seg, strlen(seg) + 1, (void **) &ppzval) == FAILURE) {
					efree(entry);
					RETURN_NULL();
				}

				properties = *ppzval;
				seg = php_strtok_r(NULL, ".", &pptr);
			}
		} else {
			if (zend_hash_find(Z_ARRVAL_P(properties), name, len + 1, (void **)&ppzval) == FAILURE) {
				efree(entry);
				RETURN_NULL();
			}
		}

		efree(entry);

		if (Z_TYPE_PP(ppzval) == IS_ARRAY) {
			/* 如果找到的值是一个数组，则将他传入yaf_config_ini_format，再经过yaf_config_ini_instance，利用找到的值生成一个对象yaf_config_ini的对象返回 */
			if ((ret = yaf_config_ini_format(getThis(), ppzval TSRMLS_CC))) {
				RETURN_ZVAL(ret, 1, 1);
			} else {
				RETURN_NULL();
			}
		} else {
			RETURN_ZVAL(*ppzval, 1, 0);
		}
	}

	RETURN_FALSE;
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::toArray(void)
*/
PHP_METHOD(yaf_config_ini, toArray) {
	/* 读取$_config直接返回 */
	zval *properties = zend_read_property(yaf_config_ini_ce, getThis(), ZEND_STRL(YAF_CONFIG_PROPERT_NAME), 1 TSRMLS_CC);
	RETURN_ZVAL(properties, 1, 0);
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::set($name, $value)
 *	Yaf_Config_Ini里面的set只是个摆饰，根本没用的
 */
PHP_METHOD(yaf_config_ini, set) {
	RETURN_FALSE;
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::__isset($name)
*/
PHP_METHOD(yaf_config_ini, __isset) {
	char * name;
	int len;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &name, &len) == FAILURE) {
		return;
	} else {
		/* 获取$_config */
		zval *prop = zend_read_property(yaf_config_ini_ce, getThis(), ZEND_STRL(YAF_CONFIG_PROPERT_NAME), 1 TSRMLS_CC);
		/* 检验$name在$_config这个数组中是否存在 */
		RETURN_BOOL(zend_hash_exists(Z_ARRVAL_P(prop), name, len + 1));
	}
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::count(void)
 *  获取$_config的元素的数量
 */
PHP_METHOD(yaf_config_ini, count) {
	zval *prop = zend_read_property(yaf_config_ini_ce, getThis(), ZEND_STRL(YAF_CONFIG_PROPERT_NAME), 1 TSRMLS_CC);
	RETURN_LONG(zend_hash_num_elements(Z_ARRVAL_P(prop)));
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::offsetUnset($index)
 *	此function也是没用的
 */
PHP_METHOD(yaf_config_ini, offsetUnset) {
	RETURN_FALSE;
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::rewind(void)
 * 	重置$_config数组的内部指针
 */
PHP_METHOD(yaf_config_ini, rewind) {
	zval *prop = zend_read_property(yaf_config_ini_ce, getThis(), ZEND_STRL(YAF_CONFIG_PROPERT_NAME), 1 TSRMLS_CC);
	zend_hash_internal_pointer_reset(Z_ARRVAL_P(prop));
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::current(void)
*/
PHP_METHOD(yaf_config_ini, current) {
	zval *prop, **ppzval, *ret;
	/* 获取$_config成员数组 */
	prop = zend_read_property(yaf_config_ini_ce, getThis(), ZEND_STRL(YAF_CONFIG_PROPERT_NAME), 1 TSRMLS_CC);
	/* 获取数组$_config内部指针当前指向的值 */
	if (zend_hash_get_current_data(Z_ARRVAL_P(prop), (void **)&ppzval) == FAILURE) {
		RETURN_FALSE;
	}

	if (Z_TYPE_PP(ppzval) == IS_ARRAY) {
		/* 如果找到的值是一个数组，则将他传入yaf_config_ini_format，再经过yaf_config_ini_instance，利用找到的值生成一个对象yaf_config_ini的对象返回 */
		if ((ret = yaf_config_ini_format(getThis(),  ppzval TSRMLS_CC))) {
			RETURN_ZVAL(ret, 1, 1);
		} else {
			RETURN_NULL();
		}
	} else {
		/* 如果不是数组则直接返回值 */
		RETURN_ZVAL(*ppzval, 1, 0);
	}
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::key(void)
*/
PHP_METHOD(yaf_config_ini, key) {
	zval *prop;
	char *string;
	ulong index;
	/* 获取$_config成员数组 */
	prop = zend_read_property(yaf_config_ini_ce, getThis(), ZEND_STRL(YAF_CONFIG_PROPERT_NAME), 0 TSRMLS_CC);
	/* 获取数组$_config内部指针当前指向的键的值和类型 */
	switch (zend_hash_get_current_key(Z_ARRVAL_P(prop), &string, &index, 0)) {
		case HASH_KEY_IS_LONG:	/* key的类型为长整型 */
			RETURN_LONG(index);
			break;
		case HASH_KEY_IS_STRING:	/* key的类型为字符串 */
			RETURN_STRING(string, 1);
			break;
		default:
			RETURN_FALSE;
	}
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::next(void)
 *	将类的成员数组$_config的内部指针往后移一位
 */
PHP_METHOD(yaf_config_ini, next) {
	zval *prop = zend_read_property(yaf_config_ini_ce, getThis(), ZEND_STRL(YAF_CONFIG_PROPERT_NAME), 1 TSRMLS_CC);
	zend_hash_move_forward(Z_ARRVAL_P(prop));
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::valid(void)
 *	检测类的成员数组$_config内部的指针是否已经到了数组的结尾
 */
PHP_METHOD(yaf_config_ini, valid) {
	zval *prop = zend_read_property(yaf_config_ini_ce, getThis(), ZEND_STRL(YAF_CONFIG_PROPERT_NAME), 1 TSRMLS_CC);
	RETURN_LONG(zend_hash_has_more_elements(Z_ARRVAL_P(prop)) == SUCCESS);
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::readonly(void)
 *	也是个废的方法，估计鸟哥兼容以前的版本吧，手册里面都没有这个方法
 */
PHP_METHOD(yaf_config_ini, readonly) {
	RETURN_TRUE;
}
/* }}} */

/** {{{ proto public Yaf_Config_Ini::__destruct
*/
PHP_METHOD(yaf_config_ini, __destruct) {
}
/* }}} */

/** {{{ proto private Yaf_Config_Ini::__clone
*/
PHP_METHOD(yaf_config_ini, __clone) {
}
/* }}} */

/** {{{ yaf_config_ini_methods
*/
zend_function_entry yaf_config_ini_methods[] = {
	PHP_ME(yaf_config_ini, __construct,	yaf_config_ini_construct_arginfo, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	/* PHP_ME(yaf_config_ini, __destruct,	NULL, ZEND_ACC_PUBLIC|ZEND_ACC_DTOR) */
	PHP_ME(yaf_config_ini, __isset, yaf_config_ini_isset_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, get,	yaf_config_ini_get_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, set, yaf_config_ini_set_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, count, yaf_config_void_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, rewind, yaf_config_void_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, current, yaf_config_void_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, next, yaf_config_void_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, valid, yaf_config_void_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, key, yaf_config_void_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, toArray, yaf_config_void_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, readonly, yaf_config_void_arginfo, ZEND_ACC_PUBLIC)
	PHP_ME(yaf_config_ini, offsetUnset, yaf_config_ini_unset_arginfo, ZEND_ACC_PUBLIC)
	PHP_MALIAS(yaf_config_ini, offsetGet, get, yaf_config_ini_rget_arginfo, ZEND_ACC_PUBLIC)
	PHP_MALIAS(yaf_config_ini, offsetExists, __isset, yaf_config_ini_isset_arginfo, ZEND_ACC_PUBLIC)
	PHP_MALIAS(yaf_config_ini, offsetSet, set, yaf_config_ini_set_arginfo, ZEND_ACC_PUBLIC)
	PHP_MALIAS(yaf_config_ini, __get, get, yaf_config_ini_get_arginfo, ZEND_ACC_PUBLIC)
	PHP_MALIAS(yaf_config_ini, __set, set, yaf_config_ini_set_arginfo, ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}
};

/* }}} */

/** {{{ YAF_STARTUP_FUNCTION
*/
YAF_STARTUP_FUNCTION(config_ini) {
	zend_class_entry ce;

	YAF_INIT_CLASS_ENTRY(ce, "Yaf_Config_Ini", "Yaf\\Config\\Ini", yaf_config_ini_methods);
	yaf_config_ini_ce = zend_register_internal_class_ex(&ce, yaf_config_ce, NULL TSRMLS_CC);	/* extends Config_Abstract */

#ifdef HAVE_SPL
	/* implements Iterator, ArrayAccess, Countable */
	zend_class_implements(yaf_config_ini_ce TSRMLS_CC, 3, zend_ce_iterator, zend_ce_arrayaccess, spl_ce_Countable);
#else
	/* implements Iterator, ArrayAccess */
	zend_class_implements(yaf_config_ini_ce TSRMLS_CC, 2, zend_ce_iterator, zend_ce_arrayaccess);
#endif

	yaf_config_ini_ce->ce_flags |= ZEND_ACC_FINAL_CLASS;

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
