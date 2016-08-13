/*
 * arch/arch/mach-sun6i/sys_config.c
 * (C) Copyright 2010-2015
 * Allwinner Technology Co., Ltd. <www.allwinnertech.com>
 *
 * sys_config utils (porting from 2.6.36)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 */
#include <linux/string.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <mach/sys_config.h>
#include <linux/slab.h>
#include <linux/bootmem.h>

#define SCRIPT_MALLOC(size)        alloc_bootmem((unsigned long)size)
#define SCRIPT_FREE(addr, size)    free_bootmem((unsigned long)addr, (unsigned long)size)


// 类型本是数字所以，此处转换为字符串
#define ITEM_TYPE_TO_STR(type)	((SCIRPT_ITEM_VALUE_TYPE_INT == (type)) ?  "int"    :	\
				((SCIRPT_ITEM_VALUE_TYPE_STR == (type))  ?  "string" :	\
				((SCIRPT_ITEM_VALUE_TYPE_PIO == (type))  ?   "gpio" : "invalid")))

/*
 * define origin main key data structure in cript buffer
 * @name: main key name, defined left of "="
 * @items: count of sub key
 * @offset: position of the value of main key, in dword
 */
#pragma pack(1) // 存储一个主键
typedef struct
{
    char name[32];              // 主键名称
    int  sub_cnt;               // 子键数
    int  offset;                // 主键值位置
} script_origin_main_key_t;
#pragma pack()

/*
 * define origin sub key data structure in cript buffer
 * @name: sub key name, defined left of "="
 * @offset: sub key value position, in dword
 * @type: type of sub key, int / string / gpio
 * @cnt:  length of the value area, in dword
 */
#pragma pack(1) //按照1字节方式进行对齐 
typedef struct  // 在脚本缓存中定义子键数据
{
    char name[32];              // 子键名称
    int  offset;                // 子键值的位置？
    struct {
        u32 cnt : 16;           // 值的区间长度
        u32 type: 16;           // 子键类型
    }pattern;
} script_origin_sub_key_t;
#pragma pack()

/*
 * define origin header of the script in cript buffer
 * @main_cnt: count of main keys
 * @version: script version
 * @main_key: fist main key
 */
#pragma pack(1) // 存储所有键值对的头
typedef struct
{
    int  main_cnt;          // 主键的个数
    int  version[3];        // 脚本版本
    script_origin_main_key_t    main_key;   // first主键
} script_origin_head_t;
#pragma pack()

/*
 * define origin gpio data structure in cript buffer
 * @gpio_name: gpio name, defined left of '='
 * @port: gpio port number, 1-PA, 2-PB, 3-PC, ...
 * @port_num: pin number in port, 0-Px0, 1-Px1, ...
 * @mul_sel: multi-function select
 * @pull: pin status,
 * @drv_level: drive level
 * @data: gpio data value
 */
#pragma pack(1)     // 记录gpio的使用情况
typedef struct {
    char    gpio_name[32];      // gpio名字
    int     port;               // gpio端口号(PA，PB，.....)
    int     port_num;           // gpio端口pin号
    int     mul_sel;            // 功能复用选择
    int     pull;               // 内部电平状态（pin状态？）
    int     drv_level;          // 驱动能力
    int     data;               // 输出电平（gpio数据值？）
} script_origin_gpio_t;
#pragma pack()

/*
 *===========================================================================================================
 * origin script defined as follows:
 *
 * |-----------------------|
 * |@main-cnt |@version[3] |            // script主要信息,script_origin_head_t
 * |-----------------------|
 * | origin main key 0:    |
 * | @name[32]             |
 * | @sub_cnt              |
 * | @offset   ------------|-----
 * |-----------------------|    |
 * | origin main key 1:    |    |        // script主键信息,script_origin_main_key_t
 * | @name[32]             |    |
 * | @sub_cnt              |    |
 * | @offset               |    |
 * |-----------------------|    |
 * | origin main key 2:    |    |
 * |      ......           |    |
 *                              |
 *                              |
 *                              |
 * |-----------------------|    |
 * | origin sub key 0:     |<---|       // script子键信息,script_origin_sub_key_t
 * | @name                 |
 * | @offset   ------------|----|
 * | @type                 |    |
 * |-----------------------|    |
 * | origin sub key 0:     |    |
 * | @name                 |    |
 * | @offset               |    |
 * | @type                 |    |
 * |-----------------------|    |
 * | origin sub key 0:     |    |
 * |    ......             |    |
 *                              |
 *                              |
 *                              |
 * |-----------------------|    |
 * | origin sub key value: |<---|
 * |    ......             |
 *
 *
 *
 *===========================================================================================================
 * script parse result organized as follows:
 *
 * |-----------------------|
 * | script_main_key_t     |
 * | @name[]               |
 * | @subkey      ---------|-------------------------------------------->|-----------------------|
 * | @subkey_val  ---------|------->|-----------------------|            | script_sub_key_t      |
 * | @gpio    -------------|------->| script_item_u         |<-----|     | @name[]               |
 * | @gpio_cnt             |        | @val                  |      ------|-@value                |
 * | @hash                 |        | @str                  |            | @type                 |
 * | @next      -----------|----    | @gpio                 |            | @hash                 |
 * |-----------------------|   |    |-----------------------|            | @next    -------------|---|
 * | script_main_key_t     |<--|    | script_item_u         |<----|      |-----------------------|   |
 * | @name[]               |        | @val                  |     |      | script_sub_key_t      |<--|
 * | @subkey               |        | @str                  |--|  |      | @name[]               |
 * | @subkey_val           |        | @gpio                 |  |  -------|-@value                |
 * | @gpio                 |        |-----------------------|  |         | @type                 |
 * | @gpio_cnt             |        | script_item_u         |  |         | @hash                 |
 * | @hash                 |        | @val                  |  |         | @next                 |
 * | @next                 |        | @str                  |  |         |-----------------------|
 * |-----------------------|        | @gpio                 |  |         | script_sub_key_t      |
 * | main key 2:           |        |-----------------------|  |         |  ......               |
 * |    ......             |        | script_item_u         |  |
 *                                  | ......                |  |
 *                                                             |--------->|-----------------------|
 *                                                                        | string                |
 *                                                                        |-----------------------|
 *===========================================================================================================
 */

#define SCRIPT_NAME_SIZE_MAX    (32)

/*
 * define script item management data
 * @name: item name, which is defined left of '='
 * @value: value of the item
 * @type: type of the item, interge / string / gpio?
 * @hash: hash value of sub key name, for searching quickly
 * @next: pointer for list
 */
typedef struct {        // 管理所有子键的开头
    char                        name[SCRIPT_NAME_SIZE_MAX];    // item名
    script_item_u               *value;      // item值
    script_item_value_type_e    type;        // item类型
    int                         hash;       // 就是把该结构所有字节加在一起的值（子键名的hash值，方便快速搜索？）
    void                        *next;      // 指向下一个子键（链表指针？）
} script_sub_key_t;

/*
 * define script main key management data
 * @name: main key name, which is defined by '[]'
 * @subkey: sub key list
 * @subkey_val: buffer for save sub keys
 * @gpio: gpio list pointer
 * @gpio_cnt: gpio conter
 * @hash: hash value of sub key name, for searching quickly
 * @next: pointer for list
 */
typedef struct {        // 管理所有主键/的头，这些结构很可能直接写到了二进制中
    char                name[SCRIPT_NAME_SIZE_MAX];    // 主键名
    script_sub_key_t    *subkey;          // 子键列表
    script_item_u       *subkey_val;      // 存储子键的缓冲
    script_item_u       *gpio;            // gpio列表指针
    int                 gpio_cnt;         // gpio计数器？（conter）
    int                 hash;            // 子键名的hash值
    void                *next;           // 链表指针
} script_main_key_t;

/*
 * define script sub key type, raw from sys_config.bin
 * @SCIRPT_PARSER_VALUE_TYPE_INVALID: invalid type
 * @SCIRPT_PARSER_VALUE_TYPE_SINGLE_WORD: int item type
 * @SCIRPT_PARSER_VALUE_TYPE_STRING: string item type
 * @SCIRPT_PARSER_VALUE_TYPE_MULTI_WORD: multi int type, not used currently
 * @SCIRPT_PARSER_VALUE_TYPE_GPIO_WORD: gpio item type
 */
typedef enum {                             // 定义脚本子键类型,来源于.bin文件
	SCIRPT_PARSER_VALUE_TYPE_INVALID = 0,  // 无效类型
	SCIRPT_PARSER_VALUE_TYPE_SINGLE_WORD,  // 整型
	SCIRPT_PARSER_VALUE_TYPE_STRING,       // 字符串型
	SCIRPT_PARSER_VALUE_TYPE_MULTI_WORD,   // 多整型
	SCIRPT_PARSER_VALUE_TYPE_GPIO_WORD     // gpio型
} script_parser_value_type_t;

static script_main_key_t   *g_script;       // 全局主键指针

// 这里hash的应该是一个结构体，不包括指针链的内容
static int hash(char *string)
{
    int     hash = 0;

    if(!string) {
        return 0;
    }

    while(*string){
        hash += *string;        // 这里将所有字符相加，得到hash数据
	string++;
    }

    return hash;
}

/**
 * port_to_index - gpio port to global index, port is from script
 * @port: gpio port group index, eg: 1 for PA, 2 for PB..., -1(0xFFFF) for axp pin
 * @port_num: port index in gpio group, eg: 0 for PA0, 1 for PA1...
 *
 * return the gpio index for the port, GPIO_INDEX_INVALID indicate err
 */
/**
 * port_to_index - 输入端口号，返回其在全局区域的索引，port is from script？？
 * @port: gpio口组索引，例如: 1代表PA，2代表PB...，-1(0xFFF)代表axp引脚
 * @port_num: 端口在gpio组中的索引，例如：0代表PA0，1代表PA1...
 *
 * 返回该端口在整个gpio口中的索引，返回GPIO_INDEX_INVALID代表错误。
 */
u32 port_to_index(u32 port, u32 port_num)       // 给我一个端口，给你一个地址
{
	/* sunxi system config pin name space and 
	 * pinctrl name space map table like this:
	 * pin define group  pin-index  pinctrl-number
	 * PA0        1      0          0
	 * PB0        2      0          32
	 * PC0        3      0          64
	 * PC1        3      1          65
	 * ...
	 * pinctrl-number = (group - 1) * 32 + pin-index
	 */
	u32 index;
	
	if (port == AXP_CFG_GRP) {
		/* valid axp gpio */   // AXP_CFG_GRP = 0xFFFF
		index = AXP_PIN_BASE + port_num;
	} else {
		/* sunxi pinctrl pin */
		index = (port - 1) * 32 + port_num;
	}    // 如果axp gpio口有效，则索引= 1024+port_num;否则，索引= （port-1）*32+port_num
	return index;
}

// 打印该主键中所有子键的信息
void dump_mainkey(script_main_key_t *pmainkey)
{
	script_sub_key_t *psubkey = NULL;
	char gpio_name[8] = {0};

	if(NULL == pmainkey || NULL == pmainkey->subkey || NULL == pmainkey->subkey_val)
		return;     // 如果主键头/主键中的子键头头为空，或者子键中没有值，直接退出

	printk("++++++++++++++++++++++++++%s++++++++++++++++++++++++++\n", __func__);
	printk("    name:      %s\n", pmainkey->name);
	printk("    sub_key:   name           type      value\n");
	psubkey = pmainkey->subkey;     // 不直接操作子键指针，以免误改动
	while(psubkey) {                // 打印出所有子键情况
		switch(psubkey->type) {     // 判断子键的类型,并打印出名称，类型，值
		case SCIRPT_ITEM_VALUE_TYPE_INT:        // int
			printk("               %-15s%-10s%d\n", psubkey->name,
				ITEM_TYPE_TO_STR(psubkey->type), psubkey->value->val);
			break;
		case SCIRPT_ITEM_VALUE_TYPE_STR:        // string
			printk("               %-15s%-10s\"%s\"\n", psubkey->name,
				ITEM_TYPE_TO_STR(psubkey->type), psubkey->value->str);
			break;
		case SCIRPT_ITEM_VALUE_TYPE_PIO:        // GPIO
			sunxi_gpio_to_name(psubkey->value->gpio.gpio, gpio_name);   // 输入gpio口全局索引，输出它原来的名字
			printk("               %-15s%-10s(gpio: %#x / %s, mul: %d, pull %d, drv %d, data %d)\n", 
				psubkey->name, ITEM_TYPE_TO_STR(psubkey->type), 
				psubkey->value->gpio.gpio, gpio_name,
				psubkey->value->gpio.mul_sel,
				psubkey->value->gpio.pull, psubkey->value->gpio.drv_level, psubkey->value->gpio.data);
			break;
            // 打印gpio所有信息
		default:                                // invalid
			printk("               %-15sinvalid type!\n", psubkey->name);
			break;
		}
		psubkey = psubkey->next;
	}
	printk("--------------------------%s--------------------------\n", __func__);
}

// 如果main_key为空，则dump所有主键，否则dump出该主键即可
// 指针为null和!指针=true有什么区别？？？？(!指针=true是不是相当于指针不指向任何对象，不还是null的意思？)
int script_dump_mainkey(char *main_key)
{
	int     main_hash = 0;
	script_main_key_t *pmainkey = g_script;

	if(NULL != main_key) {
		printk("%s: dump main_key %s\n", __func__, main_key);
		main_hash = hash(main_key);
		while(pmainkey) {
			if((pmainkey->hash == main_hash) && !strcmp(pmainkey->name, main_key)) {
				dump_mainkey(pmainkey);     // 如果hash值相同，并且主键名相同，dump出来
				return 0;
			}
			pmainkey = pmainkey->next;
		}       
		printk(KERN_ERR "%s err: main key %s not found!\n", __func__, main_key);
	} else {
		printk("%s: dump all the mainkey, \n", __func__);
		while(pmainkey) {
			printk("%s: dump main key %s\n", __func__, pmainkey->name);
			dump_mainkey(pmainkey);
			pmainkey = pmainkey->next;
		}      // 如果main_key为空，则dump出所有主键 
	}
	printk("%s exit\n", __func__);
	return 0;
}
EXPORT_SYMBOL(script_dump_mainkey);

// 获得主键中子键的值(放在item指向空间)和数据类型(返回值)
script_item_value_type_e
script_get_item(char *main_key, char *sub_key, script_item_u *item)
{
    int     main_hash, sub_hash;
    script_main_key_t   *mainkey = g_script;

    if(!main_key || !sub_key || !item || !g_script) {
        return SCIRPT_ITEM_VALUE_TYPE_INVALID;
    }       // 如果任一指针为空，则返回无效类型

    main_hash = hash(main_key);
    sub_hash = hash(sub_key);

    /* try to look for the main key from main key list */
    while(mainkey) {
        if((mainkey->hash == main_hash) && !strcmp(mainkey->name, main_key)) {
            /* find the main key */
            script_sub_key_t    *subkey = mainkey->subkey;
            while(subkey) {
                if((subkey->hash == sub_hash) && !strcmp(subkey->name, sub_key)) {
                    /* find the sub key */
                    *item = *subkey->value;
                    return subkey->type;
                }
                subkey = subkey->next;
            }

            /* no sub key defined under the main key */
            return SCIRPT_ITEM_VALUE_TYPE_INVALID;
        }
        mainkey = mainkey->next;
    }       //通过hash值和名称找到主键和其对应的子键，获得子键的值和类型

    return SCIRPT_ITEM_VALUE_TYPE_INVALID;
}
EXPORT_SYMBOL(script_get_item);

// 获得主键中gpio的链表指针(**list中)和gpio口数(返回值)
int script_get_pio_list(char *main_key, script_item_u **list)
{
    int     main_hash;
    script_main_key_t   *mainkey = g_script;

    if(!main_key || !list || !g_script) {
        return 0;
    }

    main_hash = hash(main_key);

    /* try to look for the main key from main key list */
    while(mainkey) {
        if((mainkey->hash == main_hash) && !strcmp(mainkey->name, main_key)) {
            /* find the main key */
            *list = mainkey->gpio;
            return mainkey->gpio_cnt;
        }
        mainkey = mainkey->next;
    }

    /* look for main key failed */
    return 0;
}
EXPORT_SYMBOL(script_get_pio_list);

/*
 * script_get_main_key_count
 *      get the count of main_key
 *
 * @return     the count of main_key
 */
// 获得主键的个数
unsigned int script_get_main_key_count(void)
{
	unsigned int      mainkey_count = 0;
	script_main_key_t *mainkey = g_script;
    if(!mainkey) {
    	/*  system config not initialize now */
        return 0;
    }
    
    /* count the total mainkey number */
    while(mainkey) {
        mainkey_count++;
        mainkey = mainkey->next;
    }
	return mainkey_count;
}
EXPORT_SYMBOL(script_get_main_key_count);

/*
 * script_get_main_key_name
 *      get the name of main_key by index
 *
 * @main_key_index   the index of main_key
 * @main_key_name    the buffer to store target main_key_name
 * @return     the pointer of target mainkey name
 */
// 获得第main_key_index个主键的名字
char *script_get_main_key_name(unsigned int main_key_index)
{
	unsigned int      mainkey_count = 0;
	script_main_key_t *mainkey = g_script;
    if(!mainkey) {
    	/*  system config not initialize now */
        return 0;
    }
    
    /* try to find target mainkey */
    while(mainkey) {
        if (mainkey_count == main_key_index) {
        	/* find target mainkey */
        	return mainkey->name;
        }  // 如果主键个数等于main_key索引号，则返回主键名指针 
        mainkey_count++;
        mainkey = mainkey->next;
    }
    /* invalid mainkey index for search */
	return NULL;
}
EXPORT_SYMBOL(script_get_main_key_name);

// 判断该主键是否存在
bool script_is_main_key_exist(char *main_key)
{
    int     main_hash;
    script_main_key_t   *mainkey = g_script;

    if(!main_key || !g_script) {
    	pr_err("%s(%d) err: para err, main_key %s\n", __func__, __LINE__, main_key);
        return false;
    }       // 如果main_key和mainkey中任一指针为空，则显示参数错误

    main_hash = hash(main_key);

    /* try to look for the main key from main key list */
    while(mainkey) {
        if((mainkey->hash == main_hash) && !strcmp(mainkey->name, main_key)) {
            /* find the main key */
            return true;
        }
        mainkey = mainkey->next;
    }

    /* look for main key failed */
    return false;
}
EXPORT_SYMBOL(script_is_main_key_exist);

/*
 * init script
 */
// script:就是把整合好的那个bin文件叫script，这样还可以理解过去(存储各结构体的缓存？) 
int __init script_init(void)
{
    int     i, j, count;

    script_origin_head_t        *script_hdr = __va(SYS_CONFIG_MEMBASE);

    script_origin_main_key_t    *origin_main;
    script_origin_sub_key_t     *origin_sub;

    script_main_key_t           *main_key;
    script_sub_key_t            *sub_key, *tmp_sub, swap_sub;

    script_item_u               *sub_val, *tmp_val, swap_val, *pval_temp;
        // 定义各结构体类型变量
    printk("%s enter!\n", __func__);
    if(!script_hdr) {
        printk(KERN_ERR "script buffer is NULL!\n");
        return -1;
    }       // 如果主键的头指针为空，则显示“脚本缓存为空”

    /* alloc memory for main keys */ // 为主键分配内存
    g_script = SCRIPT_MALLOC(script_hdr->main_cnt*sizeof(script_main_key_t));   //为g_script指针分配的内存大小为（主键数*script_main_key_t所占字节数）
    if(!g_script) {     
        printk(KERN_ERR "try to alloc memory for main keys!\n");
        return -1;
    }

    origin_main = &script_hdr->main_key;
    for(i=0; i<script_hdr->main_cnt; i++) {  // 复制原主键名并计算hash值
        main_key = &g_script[i];

        /* copy main key name */
        strncpy(main_key->name, origin_main[i].name, SCRIPT_NAME_SIZE_MAX);
        /* calculate hash value */
        main_key->hash = hash(main_key->name);

	if (origin_main[i].sub_cnt == 0) { // 如果某主键中没有子键，则跳过子键初始化，进入到下一主键
		/* this mainkey have no subkey, skip subkey initialize */
		main_key->subkey = NULL;
		main_key->subkey_val = NULL;
		count = 0;
		goto next_mainkey;
	}
	
        /* allock memory for sub-keys */ // 为子键分配内存
        main_key->subkey = SCRIPT_MALLOC(origin_main[i].sub_cnt*sizeof(script_sub_key_t));
        //  为main_key中的子键分配的内存大小为（该主键中的子键数*script_sub_key_t所占字节数）
        main_key->subkey_val = SCRIPT_MALLOC(origin_main[i].sub_cnt*sizeof(script_item_u));
        //  为main_key中的子键值分配的内存大小为（该主键中的子键数*script_item_u所占字节数）
        if(!main_key->subkey || !main_key->subkey_val) {
            printk(KERN_ERR "try alloc memory for sub keys failed!\n");
            goto err_out;
        }   //子键和子键值中任一指针为空，则为子键分配内存failed

        sub_key = main_key->subkey;
        sub_val = main_key->subkey_val;
        origin_sub = (script_origin_sub_key_t *)((unsigned int)script_hdr + (origin_main[i].offset<<2));//？？

        /* process sub keys */ //处理子键的名称，hash值，类型及值
        for(j=0; j<origin_main[i].sub_cnt; j++) {
            strncpy(sub_key[j].name, origin_sub[j].name, SCRIPT_NAME_SIZE_MAX);
            // 将origin_sub中的子键名复制到sub_key中，并返回sub_key
            sub_key[j].hash = hash(sub_key[j].name);
            sub_key[j].type = (script_item_value_type_e)origin_sub[j].pattern.type;
            sub_key[j].value = &sub_val[j];
            if(origin_sub[j].pattern.type == SCIRPT_PARSER_VALUE_TYPE_SINGLE_WORD) { // 如果origin_sub[j]为int型，则sub_key为int型
                sub_val[j].val = *(int *)((unsigned int)script_hdr + (origin_sub[j].offset<<2));
                sub_key[j].type = SCIRPT_ITEM_VALUE_TYPE_INT;
            } else if(origin_sub[j].pattern.type == SCIRPT_PARSER_VALUE_TYPE_STRING) { //string
                sub_val[j].str = SCRIPT_MALLOC((origin_sub[j].pattern.cnt<<2) + 1);
                memcpy(sub_val[j].str, (char *)((unsigned int)script_hdr + (origin_sub[j].offset<<2)), origin_sub[j].pattern.cnt<<2);
                // 为string型则将(char *)((unsigned int)script_hdr + (origin_sub[j].offset<<2)内存地址中的内容都拷贝到sub_val[j].str
                sub_key[j].type = SCIRPT_ITEM_VALUE_TYPE_STR;
            } else if(origin_sub[j].pattern.type == SCIRPT_PARSER_VALUE_TYPE_GPIO_WORD) {
                script_origin_gpio_t    *origin_gpio = (script_origin_gpio_t *)((unsigned int)script_hdr + (origin_sub[j].offset<<2) - 32);
		u32 gpio_tmp = port_to_index(origin_gpio->port, origin_gpio->port_num);
            //临时gpio
		if(GPIO_INDEX_INVALID == gpio_tmp) // 如果GPIO_INDEX_INVALID == gpio_tmp,则gpio配置无效，重新配置sub_val中的gpio信息。
			printk(KERN_ERR "%s:%s->%s gpio cfg invalid, please check sys_config.fex!\n",__func__,main_key->name,sub_key[j].name);
                sub_val[j].gpio.gpio = gpio_tmp;
                sub_val[j].gpio.mul_sel = (u32)origin_gpio->mul_sel;
                sub_val[j].gpio.pull = (u32)origin_gpio->pull;
                sub_val[j].gpio.drv_level = (u32)origin_gpio->drv_level;
		sub_val[j].gpio.data = (u32)origin_gpio->data;
                sub_key[j].type = SCIRPT_ITEM_VALUE_TYPE_PIO;
            } else {
                sub_key[j].type = SCIRPT_ITEM_VALUE_TYPE_INVALID;
            }
        }

        /* process gpios */
        tmp_sub = main_key->subkey;
        tmp_val = main_key->subkey_val;
        count = 0;
        for(j=0; j<origin_main[i].sub_cnt; j++) {
            if(sub_key[j].type == SCIRPT_ITEM_VALUE_TYPE_PIO) {  // 如果sub_key子键中有gpio型，则将其与main_key中的子键交换
                /* swap sub key */
                swap_sub = *tmp_sub;
                *tmp_sub = sub_key[j];
                sub_key[j] = swap_sub;
		/* swap sub key value ptr */
		pval_temp = tmp_sub->value;
		tmp_sub->value = sub_key[j].value;
		sub_key[j].value = pval_temp;
                /* swap key value */
                swap_val = *tmp_val;
                *tmp_val = main_key->subkey_val[j];
                main_key->subkey_val[j] = swap_val;
                tmp_sub++;
                tmp_val++;
                count++;
            }
        }

        /* process sub key link */
        for(j=0; j<origin_main[i].sub_cnt-1; j++) { //处理main_key中的子键链与gpio信息
            main_key->subkey[j].next = &main_key->subkey[j+1];
        }
        /* set gpio information */
next_mainkey:
        main_key->gpio = main_key->subkey_val;
        main_key->gpio_cnt = count;
	main_key->next = &g_script[i+1];
    }
    g_script[script_hdr->main_cnt-1].next = 0;

    /* dump all the item */
    //script_dump_mainkey(NULL);
    printk("%s exit!\n", __func__);
    return 0;

err_out:

    /* script init failed, release resource */ // 脚本初始化失败，释放资源
    printk(KERN_ERR "init sys_config script failed!\n");
    if(g_script) {
        for(i=0; i<script_hdr->main_cnt; i++) {
            main_key = &g_script[i];
            origin_sub = (script_origin_sub_key_t *)((unsigned int)script_hdr + (origin_main[i].offset<<2));

            if(main_key->subkey_val) {
                for(j=0; j<origin_main[i].sub_cnt; j++) {
                    if(main_key->subkey[j].type == SCIRPT_ITEM_VALUE_TYPE_STR) {
                        if (main_key->subkey_val[j].str) {
				SCRIPT_FREE(main_key->subkey_val[j].str, (origin_sub[j].pattern.cnt<<2) + 1);
			}
                    }
                }
                SCRIPT_FREE(main_key->subkey_val, sizeof(script_item_u));
            }

            if(main_key->subkey) {
                SCRIPT_FREE(main_key->subkey, sizeof(script_sub_key_t));
            }
        }

        SCRIPT_FREE(g_script, script_hdr->main_cnt*sizeof(script_main_key_t));
        g_script = 0;
    }

    return -1;
}

/* string for dump all items */   
#define DUMP_ALL_STR	"all"

typedef struct {
	char mainkey[64];
}sysfs_dump_t;

typedef struct {
	char mainkey[64];
	char subkey[64];
}sysfs_get_item_t;

static sysfs_dump_t dump_struct;
static sysfs_get_item_t get_item_struct;

int __sysfs_dump_mainkey(script_main_key_t *pmainkey, char *buf)
{
	script_sub_key_t *psubkey = NULL;
	int cnt = 0;
	char gpio_name[8] = {0};

	if(NULL == pmainkey || NULL == pmainkey->subkey || NULL == pmainkey->subkey_val)
		return 0;
    //  
	cnt += sprintf(buf + cnt, "++++++++++++++++++++++++++%s++++++++++++++++++++++++++\n", __func__);
	cnt += sprintf(buf + cnt, "    name:      %s\n", pmainkey->name);
	cnt += sprintf(buf + cnt, "    sub_key:   name           type      value\n");
	psubkey = pmainkey->subkey;
	while(psubkey) {
		switch(psubkey->type) {
        // 根据item类型，将该类型的子键名称、子键类型、子键值等信息格式化写入buf+cnt缓冲区，其中cnt为上述信息对应的字符数的和。如int型，cnt为子键名、子键类型及子键值对应的字符数相加
		case SCIRPT_ITEM_VALUE_TYPE_INT:       //int
			cnt += sprintf(buf + cnt, "               %-15s%-10s%d\n", psubkey->name,
				ITEM_TYPE_TO_STR(psubkey->type), psubkey->value->val);
			break;
		case SCIRPT_ITEM_VALUE_TYPE_STR:       // string
			cnt += sprintf(buf + cnt, "               %-15s%-10s\"%s\"\n", psubkey->name,
				ITEM_TYPE_TO_STR(psubkey->type), psubkey->value->str);
			break;
		case SCIRPT_ITEM_VALUE_TYPE_PIO:      // gpio
			sunxi_gpio_to_name(psubkey->value->gpio.gpio, gpio_name);			
			cnt += sprintf(buf + cnt, "               %-15s%-10s(gpio: %#x / %s, mul: %d, pull %d, drv %d, data %d)\n", 
				psubkey->name, ITEM_TYPE_TO_STR(psubkey->type), 
				psubkey->value->gpio.gpio, gpio_name,
				psubkey->value->gpio.mul_sel,
				psubkey->value->gpio.pull, psubkey->value->gpio.drv_level, psubkey->value->gpio.data);
			break;
		default:        //无效
			cnt += sprintf(buf + cnt, "               %-15sinvalid type!\n", psubkey->name);
			break;
		}
		psubkey = psubkey->next;
	}
	cnt += sprintf(buf + cnt, "--------------------------%s--------------------------\n", __func__);
	return cnt;
}

/**
 * show func of dump attribute.
 * @dev:     class ptr.
 * @attr:    attribute ptr.
 * @buf:     the output buf which store the dump msg
 *
 * return size written to the buf, otherwise failed
 */
static ssize_t dump_show(struct class *class, struct class_attribute *attr, char *buf) //展现dump属性的函数,返回写到缓存的大小
{
	script_main_key_t *pmainkey = g_script;
	int main_hash = 0;
	int cnt = 0;
#if 1   //执行以下程序段
	if(0 == dump_struct.mainkey[0]) {
		pr_err("%s(%d) err: please input mainkey firstly\n", __func__, __LINE__);
		return -EINVAL;
	}   //dump的第一个主键不能为空
#endif
	if(!memcmp(dump_struct.mainkey, DUMP_ALL_STR, strlen(DUMP_ALL_STR)) 
        //如果dump结构中的主键与DUMP_ALL_STR两者所对应的字节不相同或dump结构中的第一个主键为空，则dump出所有主键；否则只dump该主键  
		|| 0 == dump_struct.mainkey[0]) { /* dump all mainkey */
		pr_info("%s: dump all main keys\n", __func__);
		while(pmainkey) {
			pr_info("%s: dump main key for %s\n", __func__, pmainkey->name);
			cnt += __sysfs_dump_mainkey(pmainkey, buf + cnt);
			pmainkey = pmainkey->next;
		}
	} else {
		pr_info("%s: dump main key for %s\n", __func__, dump_struct.mainkey);
		main_hash = hash(dump_struct.mainkey);
		while(pmainkey) {
			if((pmainkey->hash == main_hash) && !strcmp(pmainkey->name, dump_struct.mainkey))
				return __sysfs_dump_mainkey(pmainkey, buf);
			pmainkey = pmainkey->next;
		}
		pr_err("%s(%d) err: main key %s not found!\n", __func__, __LINE__, dump_struct.mainkey);
	}
	return cnt;
}

/**
 * store func of dump attribute.
 * @class:   class ptr.
 * @attr:    attribute ptr.
 * @buf:     the input buf which contain the mainkey name. eg: "lcd0_para\n"
 * @size:    buf size.
 *
 * return size if success, otherwise failed
 */
static ssize_t dump_store(struct class *class, struct class_attribute *attr,
			const char *buf, size_t size)  // 存储dump属性的函数，返回缓存（包含主键名的输入缓存）大小
{
	if(strlen(buf) >= sizeof(dump_struct.mainkey)) { // 如果包含主键名的输入缓存的字节数不小于dump_struct.mainkey的字节数，则缓存太长，返回-EINVAL
		pr_err("%s(%d) err: name \"%s\" too long\n", __func__, __LINE__, buf);
		return -EINVAL;
	}
	if(0 == buf[0]) { //buf[0]为0则主键无效，返回-EINVAL
		pr_err("%s(%d) err: invalid mainkey\n", __func__, __LINE__);
		return -EINVAL;
	}
	strcpy(dump_struct.mainkey, buf);   //将buf中的内容复制到dump_struct.mainkey中
	if('\n' == dump_struct.mainkey[strlen(dump_struct.mainkey) - 1]) /* remove tail \n */ // 如果dump_struct.mainkey的末尾内容为0，则获得输入主键，返回缓存大小
		dump_struct.mainkey[strlen(dump_struct.mainkey) - 1] = 0;
	pr_info("%s: get input mainkey \"%s\"\n", __func__, dump_struct.mainkey);
	return size;
}

ssize_t get_item_show(struct class *class, struct class_attribute *attr, char *buf)
{
	script_item_value_type_e type;
	script_item_u item;
	ssize_t outsize;

	type = script_get_item(get_item_struct.mainkey, get_item_struct.subkey, &item);
	if(SCIRPT_ITEM_VALUE_TYPE_INVALID == type) {
		pr_err("%s(%d) err: script_get_item failed for \"%s\"->\"%s\"\n", __func__, __LINE__,
			get_item_struct.mainkey, get_item_struct.subkey);
		return -EINVAL;
	} else {
		pr_info("%s(%d): script_get_item return type [%s] for %s->%s\n", __func__, __LINE__,
			ITEM_TYPE_TO_STR(type), get_item_struct.mainkey, get_item_struct.subkey);
		memcpy(buf, &item, sizeof(item));   //如果dump结构中主键的子键类型有效，则打印出主键和子键，并将item所在内存地址的内容复制到buf中
		/* the extra 4bytes store item type, for sizeof(script_item_value_type_e) = 4 */
		*(u32 *)(buf + sizeof(item)) = (u32)type;   //额外的4字节缓冲空间用来存储item类型
		outsize = sizeof(item) + sizeof(u32);
		/* copy string to user space */
		if(SCIRPT_ITEM_VALUE_TYPE_STR == type) {
			strcpy(buf + outsize, item.str);    //dump结构中主键的子键为string型，则将字符串复制到用户空间
			outsize += strlen(item.str);
		} else if(SCIRPT_ITEM_VALUE_TYPE_PIO == type) { //dump结构中主键的子键为gpio型，则将gpio转化成名字并复制到用户空间
			/* convert gpio to name(eg: "PH5") and copy to user space */
			//WARN_ON(0 != sw_gpio_to_name(item.gpio.gpio, buf + outsize));
			outsize += strlen(buf + outsize);
		}
		return outsize;
	}
}

ssize_t get_item_store(struct class *class, struct class_attribute *attr,
			const char *buf, size_t size)
{
	char *last_char;

	pr_info("%s: input buf %s\n", __func__, buf);
	sscanf(buf, "%s %s", get_item_struct.mainkey, get_item_struct.subkey); 
    //从buf里读入数据按string格式写入get_item_struct.mainkey和 get_item_struct.subkey
	if(0 != strlen(get_item_struct.subkey)) {
		last_char = get_item_struct.subkey + strlen(get_item_struct.subkey) - 1;
		if('\n' == *last_char)
			*last_char = 0;
	}
	pr_info("%s: get \"%s\"->\"%s\"\n", __func__, get_item_struct.mainkey, get_item_struct.subkey);
	return size;
}

static struct class_attribute script_class_attrs[] = {
	__ATTR(dump, 0644, dump_show, dump_store),
	__ATTR(get_item, 0644, get_item_show, get_item_store),
	__ATTR_NULL,
};

static struct class script_class = {
    .name = "script",
    .owner = THIS_MODULE,
    .class_attrs = script_class_attrs,
};

static int __init script_sysfs_init(void)
{
    int status;

    memset((void*)&dump_struct, 0, sizeof(dump_struct));
    memset((void*)&get_item_struct, 0, sizeof(get_item_struct));
    /* create /sys/class/script/ */
    status = class_register(&script_class);
    if (status < 0)
        pr_err("%s: status %d\n", __func__, status);
    else
        pr_info("%s success\n", __func__);

    return status;
}
postcore_initcall(script_sysfs_init);
 
