//
// Created by inory on 11/15/25.
//

#ifndef TAGGED_NODE_PTR_H
#define TAGGED_NODE_PTR_H

namespace fiber {

template<class T>
class TaggedPtr {
    using compressed_ptr_t = std::uint64_t;

public:
    using tag_t = std::uint16_t;

private:
    union cast_unit {
        compressed_ptr_t value;
        tag_t tag[4];
    };

    static constexpr int tag_index = 3;
    static constexpr compressed_ptr_t ptr_mask = 0xffffffffffffUL; //(1L<<48L)-1;

    static T *extract_ptr(volatile compressed_ptr_t const &i) { return (T *) (i & ptr_mask); }

    static tag_t extract_tag(volatile compressed_ptr_t const &i) {
        cast_unit cu;
        cu.value = i;
        return cu.tag[tag_index];
    }

    static compressed_ptr_t pack_ptr(T *ptr, tag_t tag) {
        cast_unit ret;
        ret.value = compressed_ptr_t(ptr);
        ret.tag[tag_index] = tag;
        return ret.value;
    }

public:
    /** uninitialized constructor */
    TaggedPtr() noexcept {} //: ptr(0), tag(0)

    /** copy constructor */
    TaggedPtr(TaggedPtr const &p) = default;

    explicit TaggedPtr(T *p, tag_t t = 0) : ptr(pack_ptr(p, t)) {}

    /** unsafe set operation */
    /* @{ */
    TaggedPtr &operator=(TaggedPtr const &p) = default;

    void set(T *p, tag_t t) { ptr = pack_ptr(p, t); }
    /* @} */

    /** comparing semantics */
    /* @{ */
    bool operator==(volatile TaggedPtr const &p) volatile const { return (ptr == p.ptr); }

    bool operator!=(volatile TaggedPtr const &p) volatile const { return !operator==(p); }
    /* @} */

    /** pointer access */
    /* @{ */
    T *get_ptr() const { return extract_ptr(ptr); }

    void set_ptr(T *p) {
        tag_t tag = get_tag();
        ptr = pack_ptr(p, tag);
    }
    /* @} */

    /** tag access */
    /* @{ */
    tag_t get_tag() const { return extract_tag(ptr); }

    tag_t get_next_tag() const {
        tag_t next = (get_tag() + 1u) & (std::numeric_limits<tag_t>::max)();
        return next;
    }

    void set_tag(tag_t t) {
        T *p = get_ptr();
        ptr = pack_ptr(p, t);
    }
    /* @} */

    /** smart pointer support  */
    /* @{ */
    T &operator*() const { return *get_ptr(); }

    T *operator->() const { return get_ptr(); }

    operator bool() const { return get_ptr() != 0; }
    /* @} */

protected:
    compressed_ptr_t ptr;
};

} // namespace fiber

#endif // TAGGED_NODE_PTR_H
