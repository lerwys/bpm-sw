/*
 * Copyright (C) 2014 LNLS (www.lnls.br)
 * Author: Lucas Russo <lucas.russo@lnls.br>
 *
 * Released according to the GNU LGPL, version 3 or any later version.
 */

/* This is roughly based on the work of Alessandro Rubini and others
 * "mini-rpc", located on "https://github.com/a-rubini/mini-rpc"
 *
 * In the end, I changed much of the original code and only the main
 * idea with a couple of the original structures remain */

#include <inttypes.h>

#include "dispatch_table.h"
#include "errhand.h"

/* Undef ASSERT_ALLOC to avoid conflicting with other ASSERT_ALLOC */
#ifdef ASSERT_TEST
#undef ASSERT_TEST
#endif
#define ASSERT_TEST(test_boolean, err_str, err_goto_label, /* err_core */ ...)  \
    ASSERT_HAL_TEST(test_boolean, HAL_UTILS, "[halultis:disp_table]",           \
            err_str, err_goto_label, /* err_core */ __VA_ARGS__)

#ifdef ASSERT_ALLOC
#undef ASSERT_ALLOC
#endif
#define ASSERT_ALLOC(ptr, err_goto_label, /* err_core */ ...)                   \
    ASSERT_HAL_ALLOC(ptr, HAL_UTILS, "[hutils:disp_table]",                   \
            hutils_err_str(HUTILS_ERR_ALLOC),                               \
            err_goto_label, /* err_core */ __VA_ARGS__)

#ifdef CHECK_ERR
#undef CHECK_ERR
#endif
#define CHECK_ERR(err, err_type)                                                \
    CHECK_HAL_ERR(err, HAL_UTILS, "[hutils:disp_table]",                      \
            hutils_err_str (err_type))

static hutils_err_e _disp_table_insert (disp_table_t *self, const disp_op_t* disp_op);
static hutils_err_e _disp_table_insert_all (disp_table_t *self, const disp_op_t **disp_ops);
static hutils_err_e _disp_table_remove (disp_table_t *self, uint32_t key);
static hutils_err_e _disp_table_remove_all (disp_table_t *self);
static void _disp_table_free_item (void *data);
static hutils_err_e _disp_table_check_args (disp_table_t *self, uint32_t key,
        void *args, void **ret);
static int _disp_table_call (disp_table_t *self, uint32_t key, void *owner, void *args,
        void *ret);
static hutils_err_e _disp_table_alloc_ret (const disp_op_t *disp_op, void **ret);
static hutils_err_e _disp_table_set_ret (disp_table_t *self, uint32_t key, void **ret);

/* Disp Op Handler functions */
static disp_op_handler_t *_disp_table_lookup (disp_table_t *self, uint32_t key);
static hutils_err_e _disp_table_set_ret_op (disp_op_handler_t *disp_op_handler, void **ret);
static hutils_err_e _disp_table_cleanup_args_op (disp_op_handler_t *disp_op);

disp_table_t *disp_table_new (const disp_table_ops_t *ops)
{
    assert (ops);

    disp_table_t *self = zmalloc (sizeof *self);
    ASSERT_ALLOC (self, err_self_alloc);
    self->table_h = zhash_new ();
    ASSERT_ALLOC (self->table_h, err_table_h_alloc);
    /* Only work for strings
    zhash_autofree (self->table_h);*/
    self->ops = ops;

    return self;

err_table_h_alloc:
    free (self);
err_self_alloc:
    return NULL;
}

hutils_err_e disp_table_destroy (disp_table_t **self_p)
{
    assert (self_p);

    if (*self_p) {
        disp_table_t *self = *self_p;

        _disp_table_remove_all (self);
        self->ops = NULL;
        zhash_destroy (&self->table_h);
        free (self);
        *self_p = NULL;
    }

    return HUTILS_SUCCESS;
}

hutils_err_e disp_table_insert (disp_table_t *self, const disp_op_t* disp_op)
{
    return _disp_table_insert (self, disp_op);
}

hutils_err_e disp_table_insert_all (disp_table_t *self, const disp_op_t **disp_ops)
{
    return _disp_table_insert_all (self, disp_ops);
}

hutils_err_e disp_table_remove (disp_table_t *self, uint32_t key)
{
    return _disp_table_remove (self, key);
}

hutils_err_e disp_table_remove_all (disp_table_t *self)
{
    return _disp_table_remove_all (self);
}

hutils_err_e disp_table_fill_desc (disp_table_t *self, disp_op_t **disp_ops,
        const disp_table_func_fp *func_fps)
{
    assert (self);
    assert (disp_ops);
    assert (func_fps);

    hutils_err_e err = HUTILS_SUCCESS;

    disp_op_t **disp_ops_it = disp_ops;
    const disp_table_func_fp *func_fps_it = func_fps;

    for ( ; *disp_ops_it != NULL && *func_fps_it != NULL; ++disp_ops_it,
            ++func_fps_it) {
        /* Initialize funcp_fp field with the function pointer passed to it */
        (*disp_ops_it)->func_fp = *func_fps_it;
    }

    ASSERT_TEST ((*disp_ops_it == NULL && *func_fps_it == NULL) ,
            "Attempt to initialize the function descriptor vector with an "
            "uneven number of function pointers", err_desc_null,
            HUTILS_ERR_NULL_POINTER);

err_desc_null:
    return err;
}

hutils_err_e disp_table_check_args (disp_table_t *self, uint32_t key,
        void *args, void **ret)
{
    return _disp_table_check_args (self, key, args, ret);
}

hutils_err_e disp_table_cleanup_args (disp_table_t *self, uint32_t key)
{
    hutils_err_e err = HUTILS_SUCCESS;
    disp_op_handler_t *disp_op_handler = _disp_table_lookup (self, key);
    ASSERT_TEST (disp_op_handler != NULL, "Could not find registered key",
            err_disp_op_handler_null, HUTILS_ERR_NO_FUNC_REG);

    err = _disp_table_cleanup_args_op (disp_op_handler);

err_disp_op_handler_null:
    return err;
}

const disp_op_t *disp_table_lookup (disp_table_t *self, uint32_t key)
{
    disp_op_handler_t *disp_op_handler = _disp_table_lookup (self, key);
    return disp_op_handler->op;
}

int disp_table_call (disp_table_t *self, uint32_t key, void *owner, void *args,
        void *ret)
{
    return _disp_table_call (self, key, owner, args, ret);
}

int disp_table_check_call (disp_table_t *self, uint32_t key, void *owner, void *args,
        void **ret)
{
    hutils_err_e herr = HUTILS_SUCCESS;
    int err = -1;

    herr = _disp_table_check_args (self, key, args, ret);
    ASSERT_TEST(herr == HUTILS_SUCCESS, "Wrong arguments received",
            err_invalid_args, -1);

    /* Received arguments are OK, and the return value "ret" was allocated if the
     * ownership is ours */

    /* Do the actual work... */
    err = _disp_table_call (self, key, owner, args, *ret);

err_invalid_args:
    return err;
}

hutils_err_e disp_table_set_ret (disp_table_t *self, uint32_t key, void **ret)
{
    return _disp_table_set_ret (self, key, ret);
}

/******************************************************************************/
/************************** Local static functions ****************************/
/******************************************************************************/

static hutils_err_e _disp_table_insert (disp_table_t *self, const disp_op_t *disp_op)
{
    assert (self);
    assert (disp_op);

    DBE_DEBUG (DBG_HAL_UTILS | DBG_LVL_TRACE,
            "[hutils:disp_table] Registering function \"%s\" (%p) opcode (%u) "
            "into dispatch table\n", disp_op->name, disp_op->func_fp, disp_op->opcode);

    /* Wrap disp_op into disp_op_handler structure */
    disp_op_handler_t *disp_op_handler = disp_op_handler_new ();
    ASSERT_ALLOC (disp_op_handler, err_disp_op_handler_new);

    disp_op_handler->op = disp_op;
    disp_op_handler->ret = NULL;

    hutils_err_e herr = _disp_table_alloc_ret (disp_op_handler->op, &disp_op_handler->ret);
    ASSERT_TEST (herr == HUTILS_SUCCESS, "Return value could not be allocated",
            err_alloc_ret);

    char *key_c = hutils_stringify_hex_key (disp_op_handler->op->opcode);
    ASSERT_ALLOC (key_c, err_key_c_alloc);
    int zerr = zhash_insert (self->table_h, key_c, disp_op_handler);
    ASSERT_TEST(zerr == 0, "Could not insert item into dispatch table",
            err_insert_hash);
    /* Setup free function */
    zhash_freefn (self->table_h, key_c, _disp_table_free_item);

    free (key_c);
    return HUTILS_SUCCESS;

err_insert_hash:
    free (key_c);
err_key_c_alloc:
    _disp_table_cleanup_args_op (disp_op_handler);
err_alloc_ret:
    disp_op_handler_destroy (&disp_op_handler);
err_disp_op_handler_new:
    return HUTILS_ERR_ALLOC;
}

static void _disp_table_free_item (void *data)
{
    disp_op_handler_t *disp_op_handler = (disp_op_handler_t *) data;
    disp_op_handler_destroy (&disp_op_handler);
}

static hutils_err_e _disp_table_insert_all (disp_table_t *self, const disp_op_t **disp_ops)
{
    assert (self);
    assert (disp_ops);

    hutils_err_e err = HUTILS_SUCCESS;
    const disp_op_t** disp_op_it = disp_ops;
    DBE_DEBUG (DBG_HAL_UTILS | DBG_LVL_TRACE,
        "[hutils:disp_table] Preparing to insert function in dispatch table\n");

    for ( ; *disp_op_it != NULL; ++disp_op_it) {
        hutils_err_e err = _disp_table_insert (self, *disp_op_it);
        ASSERT_TEST(err == HUTILS_SUCCESS,
                "disp_table_insert_all: Could not insert function",
                err_disp_insert);
    }
    DBE_DEBUG (DBG_HAL_UTILS | DBG_LVL_TRACE,
            "[hutils:disp_table] Exiting insert_all\n");

err_disp_insert:
    return err;
}

static hutils_err_e _disp_table_remove (disp_table_t *self, uint32_t key)
{
    char *key_c = hutils_stringify_hex_key (key);
    ASSERT_ALLOC (key_c, err_key_c_alloc);

    /* Do a lookup first to free the return value */
    disp_op_handler_t *disp_op_handler = _disp_table_lookup (self, key);
    ASSERT_TEST (disp_op_handler != NULL, "Could not find registered key",
            err_disp_op_handler_null);

    hutils_err_e err = _disp_table_cleanup_args_op (disp_op_handler);
    ASSERT_TEST (err == HUTILS_SUCCESS, "Could not free registered return value",
            err_disp_op_handler_null);

    DBE_DEBUG (DBG_HAL_UTILS | DBG_LVL_TRACE,
        "[hutils:disp_table] Removing function (key = %u) into dispatch table\n",
        key);
    /* This will trigger the free function previously registered */
    zhash_delete (self->table_h, key_c);

    free (key_c);
    return HUTILS_SUCCESS;

err_disp_op_handler_null:
    free (key_c);
err_key_c_alloc:
    return HUTILS_ERR_ALLOC;
}

static hutils_err_e _disp_table_remove_all (disp_table_t *self)
{
    assert (self);

    zlist_t *hash_keys = zhash_keys (self->table_h);
    void * table_item = zlist_first (hash_keys);

    for ( ; table_item; table_item = zlist_next (hash_keys)) {
        _disp_table_remove (self, hutils_numerify_hex_key ((char *) table_item));
    }

    zlist_destroy (&hash_keys);
    return HUTILS_SUCCESS;
}


static hutils_err_e _disp_table_alloc_ret (const disp_op_t *disp_op, void **ret)
{
    assert (disp_op);
    hutils_err_e err = HUTILS_SUCCESS;

    if (disp_op->retval_owner == DISP_OWNER_FUNC) {
        goto err_no_ownership;
    }

    uint32_t size = DISP_GET_ASIZE(disp_op->retval);
    if (size == 0) {
        goto err_size_zero;
    }

    DBE_DEBUG (DBG_HAL_UTILS | DBG_LVL_TRACE,
            "[hutils:disp_table] Allocating %u bytes for the return value of"
            " function %s\n", DISP_GET_ASIZE(disp_op->retval), disp_op->name);

    *ret = zmalloc(DISP_GET_ASIZE(disp_op->retval));
    ASSERT_ALLOC (*ret, err_ret_alloc, HUTILS_ERR_ALLOC);

err_size_zero:
err_ret_alloc:
err_no_ownership:
    return err;
}

static hutils_err_e _disp_table_set_ret (disp_table_t *self, uint32_t key, void **ret)
{
    hutils_err_e err = HUTILS_SUCCESS;
    disp_op_handler_t *disp_op_handler = _disp_table_lookup (self, key);
    ASSERT_TEST (disp_op_handler != NULL, "Could not find registered key",
            err_disp_op_handler_null, HUTILS_ERR_NO_FUNC_REG);

    err = _disp_table_set_ret_op (disp_op_handler, ret);

err_disp_op_handler_null:
    return err;
}

static hutils_err_e _disp_table_check_args (disp_table_t *self, uint32_t key,
        void *args, void **ret)
{
    hutils_err_e err = HUTILS_SUCCESS;
    disp_op_handler_t *disp_op_handler = _disp_table_lookup (self, key);
    ASSERT_TEST (disp_op_handler != NULL, "Could not find registered key",
            err_disp_op_handler_null, HUTILS_ERR_NO_FUNC_REG);

    /* Check arguments for consistency */
    /* Call registered function to check message */
    err =  disp_table_ops_check_msg (self, disp_op_handler->op, args);
    ASSERT_TEST (err == HUTILS_SUCCESS, "Arguments received are invalid",
            err_inv_args);

    /* Point "ret" to previously allocated return value */
    err = _disp_table_set_ret_op (disp_op_handler, ret);

err_inv_args:
err_disp_op_handler_null:
    return err;
}

static int _disp_table_call (disp_table_t *self, uint32_t key, void *owner, void *args,
        void *ret)
{
    int err = 0;
    /* DBE_DEBUG (DBG_HAL_UTILS | DBG_LVL_TRACE,
            "[hutils:disp_table] Calling function (key = %u, addr = %p) from dispatch table\n",
            key, disp_op->func_fp); */
    /* The function pointer is never NULL */
    disp_op_handler_t *disp_op_handler = _disp_table_lookup (self, key);
    ASSERT_TEST (disp_op_handler != NULL, "Could not find registered key",
            err_disp_op_handler_null, -1);

    /* Check if there is a registered function */
    ASSERT_TEST (disp_op_handler->op->func_fp != NULL, "No function registered",
            err_disp_op_handler_func_fp_null, -1);

    /* Check if "ret" is pointing to something valid */
    ASSERT_TEST ((disp_op_handler->op->retval != DISP_ARG_END && ret != NULL) ||
            (disp_op_handler->op->retval == DISP_ARG_END && ret == NULL),
            "Invalid return pointer value", err_inv_ret_value_null, -1);

    err = disp_op_handler->op->func_fp (owner, args, ret);

err_inv_ret_value_null:
err_disp_op_handler_func_fp_null:
err_disp_op_handler_null:
    return err;
}

/******************************************************************************/
/*********************** Disp Op Handler functions ****************************/
/******************************************************************************/

disp_op_handler_t *disp_op_handler_new ()
{
    disp_op_handler_t *self = zmalloc (sizeof *self);
    ASSERT_ALLOC (self, err_disp_op_handler_alloc);

    self->ret = NULL;

    return self;

err_disp_op_handler_alloc:
    return NULL;
}

hutils_err_e disp_op_handler_destroy (disp_op_handler_t **self_p)
{
    assert (self_p);

    if (*self_p) {
        disp_op_handler_t *self = *self_p;

        free (self);
        *self_p = NULL;
    }

    return HUTILS_SUCCESS;
}

static disp_op_handler_t *_disp_table_lookup (disp_table_t *self, uint32_t key)
{
    disp_op_handler_t *disp_op_handler = NULL;

    char *key_c = hutils_stringify_hex_key (key);
    ASSERT_ALLOC (key_c, err_key_c_alloc);

    disp_op_handler = zhash_lookup (self->table_h, key_c);
    ASSERT_TEST (disp_op_handler != NULL, "Could not find registered function",
            err_func_p_wrapper_null);

err_func_p_wrapper_null:
    free (key_c);
err_key_c_alloc:
    return disp_op_handler;
}

static hutils_err_e _disp_table_set_ret_op (disp_op_handler_t *disp_op_handler, void **ret)
{
    assert (disp_op_handler);
    hutils_err_e err = HUTILS_SUCCESS;

    /* Check if there is a return value registered */
    if (disp_op_handler->op->retval == DISP_ARG_END) {
        *ret = NULL;
        err = HUTILS_SUCCESS;
        goto err_exit;
    }

    DBE_DEBUG (DBG_HAL_UTILS | DBG_LVL_TRACE,
            "[hutils:disp_table] _disp_table_set_ret_op: Setting return value ...\n");

    /* FIXME: This shouldn't happen, as this type of error is caught on
     * initialization of the dispatch function */
    ASSERT_ALLOC (disp_op_handler->ret, err_ret_alloc, HUTILS_ERR_ALLOC);
    *ret = disp_op_handler->ret;

    DBE_DEBUG (DBG_HAL_UTILS | DBG_LVL_TRACE,
            "[hutils:disp_table] _disp_table_set_ret_op: Return value set\n");

err_ret_alloc:
err_exit:
    return err;
}

static hutils_err_e _disp_table_cleanup_args_op (disp_op_handler_t *disp_op_handler)
{
    assert (disp_op_handler);

    if (disp_op_handler->ret) {
        if (disp_op_handler->op->retval_owner == DISP_OWNER_FUNC) {
            goto err_no_ownership;
        }

        free (disp_op_handler->ret);
        disp_op_handler->ret = NULL;
    }

err_no_ownership:
    return HUTILS_SUCCESS;
}

/************************************************************/
/********************* Generic methods API ******************/
/************************************************************/

#define CHECK_FUNC(func_p)                              \
    do {                                                \
        if(func_p == NULL) {                            \
            DBE_DEBUG (DBG_HAL_UTILS | DBG_LVL_ERR,     \
                    "[hutils:disp_table] %s\n",         \
                    hutils_err_str (HUTILS_ERR_NO_FUNC_REG)); \
            return HUTILS_ERR_NO_FUNC_REG;              \
        }                                               \
    } while(0)

#define ASSERT_FUNC(func_name)                          \
    do {                                                \
        assert (self);                                  \
        assert (self->ops);                             \
        CHECK_FUNC (self->ops->func_name);              \
    } while(0)

/* Declare wrapper for all LLIO functions API */
#define DISP_TABLE_FUNC_WRAPPER(func_name, ...)         \
{                                                       \
    ASSERT_FUNC(func_name);                             \
    return self->ops->func_name (self, ##__VA_ARGS__);  \
}

/**** Check message arguments ****/
hutils_err_e disp_table_ops_check_msg (disp_table_t *self, const disp_op_t *disp_op,
        void *args)
    DISP_TABLE_FUNC_WRAPPER(check_msg_args, disp_op, args);

