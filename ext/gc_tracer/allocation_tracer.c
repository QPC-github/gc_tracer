/*
 * allocation tracer: adds GC::Tracer::start_allocation_tracing
 *
 * By Koichi Sasada
 * created at Thu Apr 17 03:50:38 2014.
 */

#include "ruby/ruby.h"
#include "ruby/debug.h"

struct allocation_info {
    /* all of information don't need marking. */
    int living;
    VALUE flags;
    VALUE klass;
    const char *klass_path;
    size_t generation;

    /* allocation info */
    const char *path;
    unsigned long line;

    struct allocation_info *next;
};

#define KEY_PATH    (1<<1)
#define KEY_LINE    (1<<2)
#define KEY_TYPE    (1<<3)
#define KEY_CLASS   (1<<4)

#define VAL_COUNT     (1<<1)
#define VAL_TOTAL_AGE (1<<2)
#define VAL_MAX_AGE   (1<<3)
#define VAL_MIN_AGE   (1<<4)

struct traceobj_arg {
    int running;
    int keys, vals;
    st_table *aggregate_table;  /* user defined key -> [count, total_age, max_age, min_age] */
    st_table *object_table;     /* obj (VALUE)      -> allocation_info */
    st_table *str_table;        /* cstr             -> refcount */
    struct allocation_info *freed_allocation_info;
};

extern VALUE rb_mGCTracer;

static char *
keep_unique_str(st_table *tbl, const char *str)
{
    st_data_t n;

    if (str && st_lookup(tbl, (st_data_t)str, &n)) {
	char *result;

	st_insert(tbl, (st_data_t)str, n+1);
	st_get_key(tbl, (st_data_t)str, (st_data_t *)&result);

	return result;
    }
    else {
	return NULL;
    }
}

static const char *
make_unique_str(st_table *tbl, const char *str, long len)
{
    if (!str) {
	return NULL;
    }
    else {
	char *result;

	if ((result = keep_unique_str(tbl, str)) == NULL) {
	    result = (char *)ruby_xmalloc(len+1);
	    strncpy(result, str, len);
	    result[len] = 0;
	    st_add_direct(tbl, (st_data_t)result, 1);
	}
	return result;
    }
}

static void
delete_unique_str(st_table *tbl, const char *str)
{
    if (str) {
	st_data_t n;

	st_lookup(tbl, (st_data_t)str, &n);
	if (n == 1) {
	    st_delete(tbl, (st_data_t *)&str, 0);
	    ruby_xfree((char *)str);
	}
	else {
	    st_insert(tbl, (st_data_t)str, n-1);
	}
    }
}

/* file, line, type */
#define MAX_KEY_DATA 4

struct memcmp_key_data {
    int n;
    st_data_t data[4];
};

static int
memcmp_hash_compare(st_data_t a, st_data_t b)
{
    struct memcmp_key_data *k1 = (struct memcmp_key_data *)a;
    struct memcmp_key_data *k2 = (struct memcmp_key_data *)b;
    return memcmp(&k1->data[0], &k2->data[0], k1->n * sizeof(st_data_t));
}

static st_index_t
memcmp_hash_hash(st_data_t a)
{
    struct memcmp_key_data *k = (struct memcmp_key_data *)a;
    return rb_memhash(k->data, sizeof(st_data_t) * k->n);
}

static const struct st_hash_type memcmp_hash_type = {
    memcmp_hash_compare, memcmp_hash_hash
};

static struct traceobj_arg *tmp_trace_arg; /* TODO: Do not use global variables */

static struct traceobj_arg *
get_traceobj_arg(void)
{
    if (tmp_trace_arg == 0) {
	tmp_trace_arg = ALLOC_N(struct traceobj_arg, 1);
	tmp_trace_arg->running = 0;
	tmp_trace_arg->keys = 0;
	tmp_trace_arg->vals = VAL_COUNT | VAL_TOTAL_AGE | VAL_MAX_AGE | VAL_MIN_AGE;
	tmp_trace_arg->aggregate_table = st_init_table(&memcmp_hash_type);
	tmp_trace_arg->object_table = st_init_numtable();
	tmp_trace_arg->str_table = st_init_strtable();
	tmp_trace_arg->freed_allocation_info = NULL;
    }
    return tmp_trace_arg;
}

static int
free_keys_i(st_data_t key, st_data_t value, void *data)
{
    ruby_xfree((void *)key);
    return ST_CONTINUE;
}

static int
free_values_i(st_data_t key, st_data_t value, void *data)
{
    ruby_xfree((void *)value);
    return ST_CONTINUE;
}

static int
free_key_values_i(st_data_t key, st_data_t value, void *data)
{
    ruby_xfree((void *)key);
    ruby_xfree((void *)value);
    return ST_CONTINUE;
}

static void
clear_traceobj_arg(void)
{
    struct traceobj_arg * arg = get_traceobj_arg();

    st_foreach(arg->aggregate_table, free_key_values_i, 0);
    st_clear(arg->aggregate_table);
    st_foreach(arg->object_table, free_values_i, 0);
    st_clear(arg->object_table);
    st_foreach(arg->str_table, free_keys_i, 0);
    st_clear(arg->str_table);
}

static struct allocation_info *
create_allocation_info(void)
{
    return (struct allocation_info *)ruby_xmalloc(sizeof(struct allocation_info));
}

static void
free_allocation_info(struct traceobj_arg *arg, struct allocation_info *info)
{
    delete_unique_str(arg->str_table, info->path);
    delete_unique_str(arg->str_table, info->klass_path);
    ruby_xfree(info);
}

static void
newobj_i(VALUE tpval, void *data)
{
    struct traceobj_arg *arg = (struct traceobj_arg *)data;
    struct allocation_info *info;
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE obj = rb_tracearg_object(tparg);
    VALUE klass = RBASIC_CLASS(obj);
    VALUE path = rb_tracearg_path(tparg);
    VALUE line = rb_tracearg_lineno(tparg);
    VALUE klass_path = (RTEST(klass) && !OBJ_FROZEN(klass)) ? rb_class_path_cached(klass) : Qnil;
    const char *path_cstr = RTEST(path) ? make_unique_str(arg->str_table, RSTRING_PTR(path), RSTRING_LEN(path)) : NULL;
    const char *klass_path_cstr = RTEST(klass_path) ? make_unique_str(arg->str_table, RSTRING_PTR(klass_path), RSTRING_LEN(klass_path)) : NULL;

    if (st_lookup(arg->object_table, (st_data_t)obj, (st_data_t *)&info)) {
	if (info->living) {
	    /* do nothing. there is possibility to keep living if FREEOBJ events while suppressing tracing */
	}
	/* reuse info */
	delete_unique_str(arg->str_table, info->path);
	delete_unique_str(arg->str_table, info->klass_path);
    }
    else {
	info = create_allocation_info();
    }

    info->next = NULL;
    info->living = 1;
    info->flags = RBASIC(obj)->flags;
    info->klass = klass;
    info->klass_path = klass_path_cstr;
    info->generation = rb_gc_count();

    info->path = path_cstr;
    info->line = NUM2INT(line);

    st_insert(arg->object_table, (st_data_t)obj, (st_data_t)info);
}

/* file, line, type, klass */
#define MAX_KEY_SIZE 4

void
aggregator_i(void *data)
{
    size_t gc_count = rb_gc_count();
    struct traceobj_arg *arg = (struct traceobj_arg *)data;
    struct allocation_info *info = arg->freed_allocation_info;

    arg->freed_allocation_info = NULL;

    while (info) {
	struct allocation_info *next_info = info->next;
	st_data_t key, val;
	struct memcmp_key_data key_data;
	int *val_buff;
	int age = (int)(gc_count - info->generation);
	int i;

	i = 0;
	if (arg->keys & KEY_PATH) {
	    key_data.data[i++] = (st_data_t)info->path;
	}
	if (arg->keys & KEY_LINE) {
	    key_data.data[i++] = (st_data_t)info->line;
	}
	if (arg->keys & KEY_TYPE) {
	    key_data.data[i++] = (st_data_t)(info->flags & T_MASK);
	}
	if (arg->keys & KEY_CLASS) {
	    key_data.data[i++] = (st_data_t)info->klass_path;
	}
	key_data.n = i;
	key = (st_data_t)&key_data;

	if (st_lookup(arg->aggregate_table, key, &val) == 0) {
	    struct memcmp_key_data *key_buff = ruby_xmalloc(sizeof(int) + sizeof(st_data_t) * key_data.n);
	    key_buff->n = key_data.n;

	    for (i=0; i<key_data.n; i++) {
		key_buff->data[i] = key_data.data[i];
	    }
	    key = (st_data_t)key_buff;

	    /* count, total age, max age, min age */
	    val_buff = ALLOC_N(int, 4);
	    val_buff[0] = val_buff[1] = 0;
	    val_buff[2] = val_buff[3] = age;

	    if (arg->keys & KEY_PATH) keep_unique_str(arg->str_table, info->path);
	    if (arg->keys & KEY_CLASS) keep_unique_str(arg->str_table, info->klass_path);

	    st_insert(arg->aggregate_table, (st_data_t)key_buff, (st_data_t)val_buff);
	}
	else {
	    val_buff = (int *)val;
	}

	val_buff[0] += 1;
	val_buff[1] += age;
	if (val_buff[2] > age) val_buff[2] = age;
	if (val_buff[3] < age) val_buff[3] = age;

	free_allocation_info(arg, info);
	info = next_info;
    }
}

static void
move_to_freed_list(struct traceobj_arg *arg, VALUE obj, struct allocation_info *info)
{
    info->next = arg->freed_allocation_info;
    arg->freed_allocation_info = info;
    st_delete(arg->object_table, (st_data_t *)&obj, (st_data_t *)&info);
}

static void
freeobj_i(VALUE tpval, void *data)
{
    struct traceobj_arg *arg = (struct traceobj_arg *)data;
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE obj = rb_tracearg_object(tparg);
    struct allocation_info *info;

    if (arg->freed_allocation_info == NULL) {
	rb_postponed_job_register_one(0, aggregator_i, arg);
    }

    if (st_lookup(arg->object_table, (st_data_t)obj, (st_data_t *)&info)) {
	move_to_freed_list(arg, obj, info);
    }
}

static void
start_alloc_hooks(VALUE mod)
{
    VALUE newobj_hook, freeobj_hook;
    struct traceobj_arg *arg = get_traceobj_arg();

    if ((newobj_hook = rb_ivar_get(rb_mGCTracer, rb_intern("newobj_hook"))) == Qnil) {
	rb_ivar_set(rb_mGCTracer, rb_intern("newobj_hook"), newobj_hook = rb_tracepoint_new(0, RUBY_INTERNAL_EVENT_NEWOBJ, newobj_i, arg));
	rb_ivar_set(rb_mGCTracer, rb_intern("freeobj_hook"), freeobj_hook = rb_tracepoint_new(0, RUBY_INTERNAL_EVENT_FREEOBJ, freeobj_i, arg));
    }
    else {
	freeobj_hook = rb_ivar_get(rb_mGCTracer, rb_intern("freeobj_hook"));
    }

    rb_tracepoint_enable(newobj_hook);
    rb_tracepoint_enable(freeobj_hook);
}

static const char *
type_name(int type)
{
    switch (type) {
#define TYPE_NAME(t) case (t): return #t;
	TYPE_NAME(T_NONE);
	TYPE_NAME(T_OBJECT);
	TYPE_NAME(T_CLASS);
	TYPE_NAME(T_MODULE);
	TYPE_NAME(T_FLOAT);
	TYPE_NAME(T_STRING);
	TYPE_NAME(T_REGEXP);
	TYPE_NAME(T_ARRAY);
	TYPE_NAME(T_HASH);
	TYPE_NAME(T_STRUCT);
	TYPE_NAME(T_BIGNUM);
	TYPE_NAME(T_FILE);
	TYPE_NAME(T_MATCH);
	TYPE_NAME(T_COMPLEX);
	TYPE_NAME(T_RATIONAL);
	TYPE_NAME(T_NIL);
	TYPE_NAME(T_TRUE);
	TYPE_NAME(T_FALSE);
	TYPE_NAME(T_SYMBOL);
	TYPE_NAME(T_FIXNUM);
	TYPE_NAME(T_UNDEF);
	TYPE_NAME(T_NODE);
	TYPE_NAME(T_ICLASS);
	TYPE_NAME(T_ZOMBIE);
	TYPE_NAME(T_DATA);
#undef TYPE_NAME
    }
    return "unknown";
}

struct arg_and_result {
    struct traceobj_arg *arg;
    VALUE result;
};

static int
aggregate_result_i(st_data_t key, st_data_t val, void *data)
{
    struct arg_and_result *aar = (struct arg_and_result *)data;
    struct traceobj_arg *arg = aar->arg;
    VALUE result = aar->result;

    int *val_buff = (int *)val;
    struct memcmp_key_data *key_buff = (struct memcmp_key_data *)key;
    VALUE v = rb_ary_new3(4, INT2FIX(val_buff[0]), INT2FIX(val_buff[1]), INT2FIX(val_buff[2]), INT2FIX(val_buff[3]));
    VALUE k = rb_ary_new();
    int i = 0;
    static VALUE type_symbols[T_MASK] = {0};

    if (type_symbols[0] == 0) {
	int i;
	for (i=0; i<T_MASK; i++) {
	    type_symbols[i] = ID2SYM(rb_intern(type_name(i)));
	}
    }

    i = 0;
    if (arg->keys & KEY_PATH) {
	const char *path = (const char *)key_buff->data[i++];
	if (path) {
	    rb_ary_push(k, rb_str_new2(path));
	    delete_unique_str(arg->str_table, path);
	}
	else {
	    rb_ary_push(k, Qnil);
	}
    }
    if (arg->keys & KEY_LINE) {
	rb_ary_push(k, INT2FIX((int)key_buff->data[i++]));
    }
    if (arg->keys & KEY_TYPE) {
	rb_ary_push(k, type_symbols[key_buff->data[i++]]);
    }
    if (arg->keys & KEY_CLASS) {
	const char *klass_path = (const char *)key_buff->data[i++];
	if (klass_path) {
	    
	    delete_unique_str(arg->str_table, klass_path);
	}
	else {
	    rb_ary_push(k, Qnil);
	}
    }

    rb_hash_aset(result, k, v);

    return ST_CONTINUE;
}

static int
aggregate_rest_object_i(st_data_t key, st_data_t val, void *data)
{
    struct traceobj_arg *arg = (struct traceobj_arg *)data;
    struct allocation_info *info = (struct allocation_info *)val;
    move_to_freed_list(arg, (VALUE)key, info);
    return ST_CONTINUE;
}

static VALUE
aggregate_result(struct traceobj_arg *arg)
{
    struct arg_and_result aar;
    aar.result = rb_hash_new();
    aar.arg = arg;

    st_foreach(arg->object_table, aggregate_rest_object_i, (st_data_t)arg);
    aggregator_i(arg);
    st_foreach(arg->aggregate_table, aggregate_result_i, (st_data_t)&aar);
    clear_traceobj_arg();
    return aar.result;
}

static VALUE
stop_allocation_tracing(VALUE self)
{
    struct traceobj_arg * arg = get_traceobj_arg();

    if (arg->running) {
	VALUE newobj_hook = rb_ivar_get(rb_mGCTracer, rb_intern("newobj_hook"));
	VALUE freeobj_hook = rb_ivar_get(rb_mGCTracer, rb_intern("freeobj_hook"));
	rb_tracepoint_disable(newobj_hook);
	rb_tracepoint_disable(freeobj_hook);

	arg->running = 0;
    }
    else {
	rb_raise(rb_eRuntimeError, "not started yet.");
    }

    return Qnil;
}

VALUE
gc_tracer_stop_allocation_tracing(VALUE self)
{
    stop_allocation_tracing(self);
    return aggregate_result(get_traceobj_arg());
}

VALUE
gc_tracer_start_allocation_tracing(VALUE self)
{
    struct traceobj_arg * arg = get_traceobj_arg();

    if (arg->running) {
	rb_raise(rb_eRuntimeError, "can't run recursivly");
    }
    else {
	arg->running = 1;
	if (arg->keys == 0) arg->keys = KEY_PATH | KEY_LINE;
	start_alloc_hooks(rb_mGCTracer);

	if (rb_block_given_p()) {
	    rb_ensure(rb_yield, Qnil, stop_allocation_tracing, Qnil);
	    return aggregate_result(get_traceobj_arg());
	}
    }

    return Qnil;
}

VALUE
gc_tracer_setup_allocation_tracing(int argc, VALUE *argv, VALUE self)
{
    struct traceobj_arg * arg = get_traceobj_arg();

    if (arg->running) {
	rb_raise(rb_eRuntimeError, "can't change configuration during running");
    }
    else {
	int i;
	VALUE ary = rb_check_array_type(argv[0]);

	for (i=0; i<(int)RARRAY_LEN(ary); i++) {
	         if (RARRAY_AREF(ary, i) == ID2SYM(rb_intern("path"))) arg->keys |= KEY_PATH;
	    else if (RARRAY_AREF(ary, i) == ID2SYM(rb_intern("line"))) arg->keys |= KEY_LINE;
	    else if (RARRAY_AREF(ary, i) == ID2SYM(rb_intern("type"))) arg->keys |= KEY_TYPE;
	    else if (RARRAY_AREF(ary, i) == ID2SYM(rb_intern("class"))) arg->keys |= KEY_CLASS;
	    else {
		rb_raise(rb_eArgError, "not supported key type");
	    }
	}
    }

    return Qnil;
}

VALUE
gc_tracer_header_of_allocation_tracing(VALUE self)
{
    VALUE ary = rb_ary_new();
    struct traceobj_arg * arg = get_traceobj_arg();

    if (arg->keys & KEY_PATH) rb_ary_push(ary, ID2SYM(rb_intern("path")));
    if (arg->keys & KEY_LINE) rb_ary_push(ary, ID2SYM(rb_intern("line")));
    if (arg->keys & KEY_TYPE) rb_ary_push(ary, ID2SYM(rb_intern("type")));
    if (arg->keys & KEY_CLASS) rb_ary_push(ary, ID2SYM(rb_intern("class")));

    if (arg->vals & VAL_COUNT) rb_ary_push(ary, ID2SYM(rb_intern("count")));
    if (arg->vals & VAL_TOTAL_AGE) rb_ary_push(ary, ID2SYM(rb_intern("total_age")));
    if (arg->vals & VAL_MAX_AGE) rb_ary_push(ary, ID2SYM(rb_intern("max_age")));
    if (arg->vals & VAL_MIN_AGE) rb_ary_push(ary, ID2SYM(rb_intern("min_age")));

    return ary;
}

