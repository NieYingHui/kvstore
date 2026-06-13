#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "../../kvstore.h"

Node* createNode(int level, char *key, char *value);
int kvs_skiplist_create(SkipList* skipList);
int randomLevel();
int kvs_skiplist_set(SkipList* skipList, char *key, char *value);
int kvs_skiplist_mod(SkipList* skipList, char *key, char *value);
void display(SkipList* skipList);
void kvs_skiplist_destroy(SkipList* skipList);
char* kvs_skiplist_get(SkipList* skipList, char *key);
int kvs_skiplist_del(SkipList* skipList, char *key);
int kvs_skiplist_exist(SkipList* skipList, char *key);

// 创建节点
Node* createNode(int level, char *key, char *value) {
    Node* newNode = (Node*)kvs_malloc(sizeof(Node));
    if (!newNode) return NULL;
    
    // 初始化 forward 数组
    newNode->forward = (Node**)kvs_malloc((level + 1) * sizeof(Node*));
    if (!newNode->forward) {
        kvs_free(newNode);
        return NULL;
    }
    
    for (int i = 0; i <= level; ++i) {
        newNode->forward[i] = NULL;
    }
    
#if ENABLE_KEY_SKIPLIST
    // 处理头节点（key 为 NULL 的情况）
    if (key == NULL) {
        newNode->key = NULL;
    } else {
        newNode->key = (char*)kvs_malloc(strlen(key) + 1);
        if (!newNode->key) {
            kvs_free(newNode->forward);
            kvs_free(newNode);
            return NULL;
        }
        strcpy(newNode->key, key);
    }
    
    if (value == NULL) {
        newNode->value = NULL;
    } else {
        newNode->value = (char*)kvs_malloc(strlen(value) + 1);
        if (!newNode->value) {
            if (newNode->key) kvs_free(newNode->key);
            kvs_free(newNode->forward);
            kvs_free(newNode);
            return NULL;
        }
        strcpy(newNode->value, value);
    }
#else
    // 整数模式（原代码）
    newNode->key = key;
    newNode->value = value;
#endif
    
    return newNode;
}

// 创建跳表
int kvs_skiplist_create(SkipList* skipList) {
    // SkipList* skipList = (SkipList*)malloc(sizeof(SkipList));
    if (!skipList) return -1;
    
    skipList->level = 0;
    
    // 头节点的 key 和 value 设为 NULL
    skipList->header = createNode(MAX_LEVEL, NULL, NULL);
    if (!skipList->header) {
        return -2;
    }
    
    for (int i = 0; i <= MAX_LEVEL; ++i) {
        skipList->header->forward[i] = NULL;
    }
    
    return 0; 
}

// 随机层级生成
int randomLevel() {
    int level = 0;
    while (rand() < RAND_MAX / 2 && level < MAX_LEVEL)
        level++;
    return level;
}

// 插入操作
int kvs_skiplist_set(SkipList* skipList, char *key, char *value) {
    if (!skipList || !key || !value) return false;
    
    Node* update[MAX_LEVEL + 1];
    Node* current = skipList->header;

    // 从最高层开始查找插入位置
    for (int i = skipList->level; i >= 0; --i) {
        while (current->forward[i] != NULL && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0];

    // 如果key已存在，返回false，不执行任何操作
    if (current != NULL && current->key != NULL && 
        strcmp(current->key, key) == 0) {
        // printf("Insert failed: Key %s already exists\n", key);
        return 1;
    }
    
    // key不存在，执行插入
    int level = randomLevel();
    
    // 如果新节点的层级大于当前跳表层级，更新跳表层级
    if (level > skipList->level) {
        for (int i = skipList->level + 1; i <= level; ++i)
            update[i] = skipList->header;
        skipList->level = level;
    }

    Node* newNode = createNode(level, key, value);
    if (!newNode) return -1;

    // 在各层级插入新节点
    for (int i = 0; i <= level; ++i) {
        newNode->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = newNode;
    }
    
    // printf("Inserted key %s with value %s\n", key, value);
    return 0;
}

int kvs_skiplist_mod(SkipList* skipList, char *key, char *value) {
    if (!skipList || !key || !value) return false;
    
    Node* current = skipList->header;

    // 从最高层开始查找
    for (int i = skipList->level; i >= 0; --i) {
        while (current->forward[i] != NULL && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }

    current = current->forward[0];

    // 如果找到key，修改value
    if (current != NULL && current->key != NULL && 
        strcmp(current->key, key) == 0) {
        
        // 释放旧value的内存
        if (current->value) kvs_free(current->value);
        
        // 分配新value的内存
        current->value = (char*)kvs_malloc(strlen(value) + 1);
        if (!current->value) {
            return -1;
        }
        strcpy(current->value, value);
        
        // printf("Modified key %s to value %s\n", key, value);
        return 0;
    } else {
        // printf("Modify failed: Key %s not found\n", key);
        return 1;
    }
}

// 显示操作
void display(SkipList* skipList) {
    if (!skipList) return;
    
    // printf("Skip List:\n");
    
    for (int i = 0; i <= skipList->level; ++i) {
        Node* node = skipList->header->forward[i];
        // printf("Level %d: ", i);
        
        while (node != NULL && node->key != NULL) {
            // printf("%s ", node->key);
            node = node->forward[i];
        }
        
        // printf("\n");
    }
}

// 查找操作
char* kvs_skiplist_get(SkipList* skipList, char *key) {
    if (!skipList || !key) return NULL;
    
    Node* current = skipList->header;

    for (int i = skipList->level; i >= 0; --i) {
        while (current->forward[i] != NULL && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }

    current = current->forward[0];

    if (current != NULL && current->key != NULL && 
        strcmp(current->key, key) == 0) {
        // printf("Key %s found with value %s\n", key, current->value);
        return current->value;
    } else {
        // printf("Key %s not found\n", key);
        return NULL;
    }
}

// 删除操作
int kvs_skiplist_del(SkipList* skipList, char *key) {
    if (!skipList || !key) return -1;
    
    Node* update[MAX_LEVEL + 1];
    Node* current = skipList->header;

    for (int i = skipList->level; i >= 0; --i) {
        while (current->forward[i] != NULL && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0];

    if (current != NULL && strcmp(current->key, key) == 0) {
        for (int i = 0; i <= skipList->level; ++i) {
            if (update[i]->forward[i] != current)
                break;
            update[i]->forward[i] = current->forward[i];
        }

        // 释放内存
#if ENABLE_KEY_SKIPLIST
    if (current->key) kvs_free(current->key);
    if (current->value) kvs_free(current->value);
#endif
    kvs_free(current->forward);
    kvs_free(current);

        // 更新跳表层级
        while (skipList->level > 0 && 
               skipList->header->forward[skipList->level] == NULL) {
            skipList->level--;
        }

        // printf("Deleted key %s\n", key);
        return 0;
    } else {
        // printf("Key %s not found for deletion\n", key);
        return 1;
    }
}

// 释放跳表
void kvs_skiplist_destory(SkipList* skipList) {
    if (!skipList) return;
    
    Node* current = skipList->header->forward[0];
    while (current) {
        Node* temp = current;
        current = current->forward[0];
        
#if ENABLE_KEY_SKIPLIST
        if (temp->key) kvs_free(temp->key);
        if (temp->value) kvs_free(temp->value);
#endif
        kvs_free(temp->forward);
        kvs_free(temp);
    }
    
    if (skipList->header) {
        kvs_free(skipList->header->forward);
        kvs_free(skipList->header);
    }
    skipList->header = NULL;
    skipList->level = 0;
}

int kvs_skiplist_exist(SkipList* skipList, char *key) {

    char *value = kvs_skiplist_get(skipList, key);
    if (!value) return 1;  // 不存在
    return 0;
}

#if 0
int main() {
    // 初始化随机数种子
    srand(time(NULL));
    
    SkipList* skipList = kvs_skiplist_create();
    if (!skipList) {
        printf("Failed to create skip list\n");
        return -1;
    }

    kvs_skiplist_set(skipList, "Teacher", "King");
    // kvs_skiplist_set(skipList, "carrot", "vegetable");
    // kvs_skiplist_set(skipList, "banana", "fruit");
    // kvs_skiplist_set(skipList, "potato", "vegetable");

    display(skipList);

    kvs_skiplist_get(skipList, "Teacher");
    // kvs_skiplist_mod(skipList, "banana", "yellow fruit");
    // kvs_skiplist_get(skipList, "banana");

    // deleteNode(skipList, "carrot");
    // deleteNode(skipList, "grape");

    // display(skipList);

    printf("Exist apple: %d\n", kvs_skiplist_exist(skipList, "apple"));
    
    // 清理内存
    kvs_skiplist_destory(skipList);
    
    return 0;
}

#endif