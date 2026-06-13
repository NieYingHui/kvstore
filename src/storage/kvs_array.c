// 存储文件  用array存储

// 单例模式 singleton

#include <stdio.h>
#include <string.h>

#include "../../kvstore.h"




int kvs_array_create(kvs_array_t *inst);
void kvs_array_destroy(kvs_array_t *inst);
int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char* kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);


// 0， 成功    < 0 失败
int kvs_array_create(kvs_array_t *inst) {
    if(!inst) return -1;

    if(inst->table) {
        printf("table has alloc\n");
        return -1;
    }
    inst->table = kvs_malloc(KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
    if(!inst->table) {
        printf("alloc table failed\n");
        return -1;
    }

    inst->idx = 0;
    inst->total = 0;
    return 0;
}


void kvs_array_destroy(kvs_array_t *inst) {
    if(!inst) return ;

    if(inst->table) {
        kvs_free(inst->table);
    }
}



/*
 * @return: <0, error; NULL, exist; 
 */
char* kvs_array_get(kvs_array_t *inst, char *key){
    if(inst == NULL || key == NULL) return NULL;

    int i = 0;
    for(;i < KVS_MAX_TOKENS; i++){
        if(inst->table[i].key == NULL) continue;
        if(strcmp(inst->table[i].key, key) == 0) return inst->table[i].value;
    }

    return NULL;
}


/*
 * @return: <0, error; =0, success; >0, exist
 */
int kvs_array_set(kvs_array_t *inst, char *key, char *value){
    if(inst == NULL || inst->table == NULL) return -1;
    if(key == NULL || value == NULL) return -1;

    char *str = kvs_array_get(inst, key);
    if(str) return 1; // key已存在

    size_t klen = strlen(key);
    char *kcopy = kvs_malloc(klen + 1); // + 1 是为了在字符串末尾添加空字符(\0)
    if(kcopy == NULL) return -2; // 内存分配失败 返回-2
    memcpy(kcopy, key, klen + 1);

    size_t vlen = strlen(value);
    char *vcopy = kvs_malloc(vlen + 1);
    if(vcopy == NULL) {
        kvs_free(kcopy);
        return -2;
    }
    memcpy(vcopy, value, vlen + 1);

    /*
     * 重要：inst->total 是“高水位”，并不代表当前已用槽位数。
     * 当数组曾经达到满载（total==KVS_ARRAY_SIZE）后再发生 DEL，会留下空洞。
     * 旧实现会在 total==KVS_MAX_TOKENS 时直接失败，导致即使有空洞也无法 SET。
     * 这里改为全表扫描寻找空槽位，找不到才失败。
     */
    for (int i = 0; i < KVS_ARRAY_SIZE; i++) {
        if (inst->table[i].key == NULL) {
            inst->table[i].key = kcopy;
            inst->table[i].value = vcopy;
            if (i >= inst->total) {
                inst->total = i + 1;
            }
            return 0;
        }
    }

    /* 表满：回收本次分配并返回失败 */
    kvs_free(kcopy);
    kvs_free(vcopy);
    return -1;

}



/*
 * @return < 0, error;  =0,  success; >0, no exist
 */
int kvs_array_del(kvs_array_t *inst, char *key){

    if(inst == NULL || key == NULL) return -1;

    int i = 0;
    for(;i < inst->total; i++){
        if(inst->table[i].key == NULL) continue;

        if(strcmp(inst->table[i].key, key) == 0){
            kvs_free(inst->table[i].key);
            inst->table[i].key = NULL;

            kvs_free(inst->table[i].value);
            inst->table[i].value = NULL;

            // error: > 1024
			// if (inst->total -1 == i) {
			// 	inst->total --;
			// }

            return 0; // 成功删除
        }
    }
    return i; // 不存在
}


/*
 * @return : < 0, error; =0, success; >0, no exist 
 */
int kvs_array_mod(kvs_array_t *inst, char *key, char *value){
    if(inst == NULL || key == NULL || value == NULL) return -1;

// error: > 1024
	if (inst->total == 0) {  // 没有元素
		return KVS_ARRAY_SIZE;
	}
    int i = 0;
    for(; i < inst->total; i++){
        if(inst->table[i].key == NULL) continue;

        if(strcmp(inst->table[i].key, key) == 0){

            kvs_free(inst->table[i].value);

            size_t vlen = strlen(value);
            char *vcopy = kvs_malloc(vlen + 1);
            if(vcopy == NULL) return -1;
            memcpy(vcopy, value, vlen + 1);

            inst->table[i].value = vcopy;
            return 0;
        }
    }

    return i;
}

/*
 * @return 0: exist, 1: no exist
 */
int kvs_array_exist(kvs_array_t *inst, char *key){
    if(inst == NULL || key == NULL) return -1;

    char *str = kvs_array_get(inst, key);
    if(!str) return 1;// 不存在

    return 0;  // 存在
}