#include "skiplist.h"
#include <cassert>
#include <string>
#include <functional>

void RegisterTest(const std::string& name, std::function<void()> fn);

namespace {

struct StringCmp {
    int operator()(const std::string& a, const std::string& b) const {
        if (a < b) return -1;
        if (a > b) return 1;
        return 0;
    }
};

void TestSkipListBasic() {
    StringCmp cmp;
    lsmkv::SkipList<std::string, StringCmp> list(cmp);

    assert(!list.Contains("hello"));
    list.Insert("hello");
    list.Insert("world");
    list.Insert("apple");
    list.Insert("banana");

    assert(list.Contains("hello"));
    assert(list.Contains("world"));
    assert(list.Contains("apple"));
    assert(list.Contains("banana"));
    assert(!list.Contains("cherry"));

    auto iter = list.NewIterator();
    iter.SeekToFirst();
    assert(iter.Valid() && iter.key() == "apple");
    iter.Next();
    assert(iter.Valid() && iter.key() == "banana");
    iter.Next();
    assert(iter.Valid() && iter.key() == "hello");
    iter.Next();
    assert(iter.Valid() && iter.key() == "world");
    iter.Next();
    assert(!iter.Valid());
}

struct Register {
    Register() {
        RegisterTest("SkipListBasic", &TestSkipListBasic);
    }
} reg;

} // namespace
