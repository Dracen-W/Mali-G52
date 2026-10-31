/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal read-only flattened-device-tree reader for the host tests. This is
 * test scaffolding, not driver code: it exists so NativeMaliBoard is checked
 * against the real bytes of the phone's DTB instead of hand-built lookalikes.
 * Only the structure block and the strings block are read; nothing is parsed
 * into a tree and nothing is written. */
#ifndef NATIVE_MALI_TEST_FDT_H
#define NATIVE_MALI_TEST_FDT_H
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

class Fdt final {
public:
    struct Property {
        const uint8_t *data=nullptr;
        size_t bytes=0;
    };
    bool open(const char *path)
    {
        std::FILE *file=std::fopen(path,"rb");
        if(!file)return false;
        std::fseek(file,0,SEEK_END);
        const long size=std::ftell(file);
        std::fseek(file,0,SEEK_SET);
        if(size<40) {std::fclose(file);return false;}
        file_.resize(size_t(size));
        const size_t read=std::fread(file_.data(),1,file_.size(),file);
        std::fclose(file);
        if(read!=file_.size())return false;
        const uint8_t *p=file_.data();
        if(be32(p)!=0xd00dfeedu)return false;
        const uint32_t total=be32(p+4);
        const uint32_t version=be32(p+20);
        if(total>file_.size() || version<17)return false;
        total_=total;
        structure_=be32(p+8);
        strings_=be32(p+12);
        strings_bytes_=be32(p+32);
        if(structure_>=total_ || strings_>total_ ||
            strings_bytes_>total_-strings_)return false;
        return true;
    }
    bool nodeExists(const char *path) const
    {
        size_t inside=0;
        return findNode(path,&inside);
    }
    bool property(const char *path,const char *name,Property *out) const
    {
        size_t inside=0;
        if(!findNode(path,&inside))return false;
        return propertyAt(inside,name,out);
    }
private:
    static uint32_t be32(const uint8_t *p)
    {
        return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
    }
    static void align4(size_t &at) {at=(at+3)&~size_t(3);}
    bool inStruct(size_t at,size_t bytes=4) const
    {
        return at<=total_ && bytes<=total_-at && at>=structure_;
    }
    /* `at` points at a BEGIN_NODE token; consumes it and its name. */
    bool enterNode(size_t &at,std::string &path) const
    {
        if(!inStruct(at))return false;
        if(be32(file_.data()+at)!=1)return false;
        at+=4;
        const size_t available=total_-at;
        const char *name=reinterpret_cast<const char *>(file_.data()+at);
        const size_t length=strnlen(name,available);
        if(length==available)return false;
        if(length) {
            path+='/';
            path.append(name,length);
        }
        at+=length+1;
        align4(at);
        return inStruct(at);
    }
    bool skipNode(size_t &at) const
    {
        size_t depth=0;
        while(inStruct(at)) {
            const uint32_t token=be32(file_.data()+at);
            at+=4;
            if(token==1) { /* BEGIN_NODE */
                const size_t available=total_-at;
                const char *name=reinterpret_cast<const char *>(file_.data()+at);
                const size_t length=strnlen(name,available);
                if(length==available)return false;
                at+=length+1;
                align4(at);
                ++depth;
            } else if(token==2) { /* END_NODE */
                if(!--depth)return true;
            } else if(token==3) { /* PROP */
                if(!inStruct(at,8))return false;
                const size_t length=be32(file_.data()+at);
                at+=8;
                if(length>total_-at)return false;
                at+=length;
                align4(at);
            } else if(token==4) { /* NOP */
            } else
                return false;
        }
        return false;
    }
    bool walk(size_t &at,std::string &path,const char *target,size_t *inside) const
    {
        if(!enterNode(at,path))return false;
        if(path==target) {
            *inside=at;
            return true;
        }
        while(inStruct(at)) {
            const uint32_t token=be32(file_.data()+at);
            if(token==1) {
                const size_t saved=path.size();
                if(walk(at,path,target,inside))return true;
                path.resize(saved); /* the child did not match: undo its name */
            } else if(token==2) {
                at+=4;
                return false;
            } else if(token==3) {
                at+=4;
                if(!inStruct(at,8))return false;
                const size_t length=be32(file_.data()+at);
                at+=8;
                if(length>total_-at)return false;
                at+=length;
                align4(at);
            } else if(token==4) {
                at+=4;
            } else
                return false;
        }
        return false;
    }
    bool findNode(const char *path,size_t *inside) const
    {
        if(!path || path[0]!='/')return false;
        size_t at=structure_;
        std::string prefix;
        /* The root node's name is empty, so its path is "" and its children's
         * paths start with '/'. */
        return walk(at,prefix,path,inside);
    }
    bool propertyAt(size_t at,const char *name,Property *out) const
    {
        while(inStruct(at)) {
            const uint32_t token=be32(file_.data()+at);
            if(token==2)return false; /* END_NODE */
            if(token==1) {
                if(!skipNode(at))return false;
                continue;
            }
            at+=4;
            if(token==4)continue;
            if(token!=3)return false;
            if(!inStruct(at,8))return false;
            const size_t length=be32(file_.data()+at);
            const size_t name_offset=be32(file_.data()+at+4);
            at+=8;
            if(name_offset>=strings_bytes_ || length>total_-at)return false;
            const char *candidate=reinterpret_cast<const char *>(file_.data()+strings_+name_offset);
            const size_t available=strings_bytes_-name_offset;
            if(strnlen(candidate,available)==std::strlen(name) &&
                std::strncmp(candidate,name,available)==0) {
                if(!out)return false;
                out->data=file_.data()+at;
                out->bytes=length;
                return true;
            }
            at+=length;
            align4(at);
        }
        return false;
    }
    std::vector<uint8_t> file_;
    size_t total_=0,structure_=0,strings_=0,strings_bytes_=0;
};
#endif
