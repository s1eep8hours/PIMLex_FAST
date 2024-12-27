#ifndef _TYPE_DEFINE_H_
#define _TYPE_DEFINE_H_

#include <iostream>
#include <stdint.h>
#include <string.h>

class string_payload{
    public:
    char real_payload[32];
    string_payload(){
        // memset(real_payload, '0', 32);
    }
    string_payload(int i){
        memset(real_payload, i, 32);
    }
    string_payload& operator =(const string_payload& str)//赋值运算符 
    {
        memcpy(this->real_payload, str.real_payload, 32);
        return *this;
    }
    
    void print(){
        std::cout << real_payload[0] << std::endl;
    }
};

#endif