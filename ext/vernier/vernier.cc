#include <iostream>
#include <vector>
#include <memory>
#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "vernier.hh"
#include "stack.hh"
#include "ruby/debug.h"

using namespace std;

#define numberof(array) ((int)(sizeof(array) / sizeof((array)[0])))

static VALUE rb_mVernier;

struct retained_collector {
    int allocated_objects = 0;
    int freed_objects = 0;

    std::unordered_set<VALUE> unique_frames;
    std::unordered_map<VALUE, std::unique_ptr<Stack>> object_frames;
};

struct TraceArg {
    rb_trace_arg_t *tparg;
    VALUE obj;
    VALUE path;
    VALUE line;
    VALUE mid;
    VALUE klass;

    TraceArg(VALUE tpval) {
        tparg = rb_tracearg_from_tracepoint(tpval);
        obj = rb_tracearg_object(tparg);
        path = rb_tracearg_path(tparg);
        line = rb_tracearg_lineno(tparg);
        mid = rb_tracearg_method_id(tparg);
        klass = rb_tracearg_defined_class(tparg);
    }
};

static retained_collector _collector;

static VALUE tp_newobj;
static VALUE tp_freeobj;
static void
newobj_i(VALUE tpval, void *data) {
    retained_collector *collector = static_cast<retained_collector *>(data);
    TraceArg tp(tpval);
    collector->allocated_objects++;

    VALUE frames_buffer[2048];
    int lines_buffer[2048];
    int n = rb_profile_frames(0, 2048, frames_buffer, lines_buffer);

    for (int i = 0; i < n; i++) {
        collector->unique_frames.insert(frames_buffer[i]);
    }

    collector->object_frames.emplace(
            tp.obj,
            make_unique<Stack>(frames_buffer, lines_buffer, n)
            );
}

static void
freeobj_i(VALUE tpval, void *data) {
    retained_collector *collector = static_cast<retained_collector *>(data);
    TraceArg tp(tpval);
    collector->freed_objects++;

    collector->object_frames.erase(tp.obj);
}


static VALUE
trace_retained_start(VALUE self) {
    retained_collector *collector = &_collector;

    tp_newobj = rb_tracepoint_new(0, RUBY_INTERNAL_EVENT_NEWOBJ, newobj_i, collector);
    tp_freeobj = rb_tracepoint_new(0, RUBY_INTERNAL_EVENT_FREEOBJ, freeobj_i, collector);

    rb_tracepoint_enable(tp_newobj);
    rb_tracepoint_enable(tp_freeobj);

    return Qtrue;
}

#define sym(name) ID2SYM(rb_intern_const(name))

// HACK: This isn't public, but the objspace ext uses it
extern "C" size_t rb_obj_memsize_of(VALUE);

static const char *
ruby_object_type_name(VALUE obj) {
    enum ruby_value_type type = rb_type(obj);

#define TYPE_CASE(x) case (x): return (#x)

    // Many of these are impossible, but it's easier to just include them
    switch (type) {
        TYPE_CASE(T_OBJECT);
        TYPE_CASE(T_CLASS);
        TYPE_CASE(T_MODULE);
        TYPE_CASE(T_FLOAT);
        TYPE_CASE(T_STRING);
        TYPE_CASE(T_REGEXP);
        TYPE_CASE(T_ARRAY);
        TYPE_CASE(T_HASH);
        TYPE_CASE(T_STRUCT);
        TYPE_CASE(T_BIGNUM);
        TYPE_CASE(T_FILE);
        TYPE_CASE(T_DATA);
        TYPE_CASE(T_MATCH);
        TYPE_CASE(T_COMPLEX);
        TYPE_CASE(T_RATIONAL);

        TYPE_CASE(T_NIL);
        TYPE_CASE(T_TRUE);
        TYPE_CASE(T_FALSE);
        TYPE_CASE(T_SYMBOL);
        TYPE_CASE(T_FIXNUM);
        TYPE_CASE(T_UNDEF);

        TYPE_CASE(T_IMEMO);
        TYPE_CASE(T_NODE);
        TYPE_CASE(T_ICLASS);
        TYPE_CASE(T_ZOMBIE);
        TYPE_CASE(T_MOVED);

        default:
        return "unknown type";
    }
#undef TYPE_CASE
}

static VALUE
trace_retained_stop(VALUE self) {
    rb_tracepoint_disable(tp_newobj);
    rb_tracepoint_disable(tp_freeobj);

    retained_collector *collector = &_collector;

    std::stringstream ss;

    for (auto& it: collector->object_frames) {
        VALUE obj = it.first;
        const Stack &stack = *it.second;

        for (int i = stack.size() - 1; i >= 0; i--) {
            const Frame &frame = stack.frame(i);
            ss << frame;
            if (i > 0) ss << ";";
        }
        ss << ";" << ruby_object_type_name(obj);
        ss << " " << rb_obj_memsize_of(obj) << endl;
    }

    std::string s = ss.str();
    VALUE str = rb_str_new(s.c_str(), s.size());

    return str;
}

static void
retained_collector_mark(void *data) {
    retained_collector *collector = static_cast<retained_collector *>(data);

    // We don't mark the objects, but we MUST mark the frames, otherwise they
    // can be garbage collected.
    // This may lead to method entries being unnecessarily retained.
    for (VALUE frame: collector->unique_frames) {
        rb_gc_mark(frame);
    }
}

extern "C" void
Init_vernier(void)
{
  rb_mVernier = rb_define_module("Vernier");

  rb_define_module_function(rb_mVernier, "trace_retained_start", trace_retained_start, 0);
  rb_define_module_function(rb_mVernier, "trace_retained_stop", trace_retained_stop, 0);

  static VALUE gc_hook = Data_Wrap_Struct(rb_cObject, retained_collector_mark, NULL, &_collector);
  rb_global_variable(&gc_hook);
}