#include <bits/stdc++.h>
using namespace std;

#include "../include/RedisDatabase.h"

RedisDatabase& RedisDatabase::getInstance() {
    static RedisDatabase instance;

    return instance;
}

bool RedisDatabase::dump(const string& filename) {
    return true;
}

bool RedisDatabase::load(const string& filename) {
    return true;
}