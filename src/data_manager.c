/**
 * @file data_manager.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief
 *
 * @copyright
 * Copyright 2015 Cisco Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "data_manager.h"
#include "sr_common.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <pthread.h>

/*
 * @brief Data manager context holding loaded schemas, data trees
 * and corresponding locks
 */
typedef struct dm_ctx_s {
    char *schema_search_dir;      /**< location where schema files are located */
    char *data_search_dir;        /**< location where data files are located */
    struct ly_ctx *ly_ctx;        /**< libyang context holding all loaded schemas */
    pthread_rwlock_t lyctx_lock;  /**< rwlock to access ly_ctx */
} dm_ctx_t;

/**
 * @brief Structure that holds Data Manager's per-session context.
 */
typedef struct dm_session_s {
    sr_datastore_t datastore;       /**< datastore to which the session is tied */
    sr_btree_t *session_modules;    /**< binary holding session copies of data models */
    char *error_msg;                /**< description of the last error */
    char *error_xpath;              /**< xpath of the last error if applicable */
    dm_sess_op_t *operations;       /**< list of operations performed in this session */
    size_t oper_count;              /**< number of performed operation */
    size_t oper_size;               /**< number of allocated operations */
} dm_session_t;

/**
 * @brief Info structure for the node holds the state of the running data store.
 * (It will hold information about notification subscriptions.)
 */
typedef struct dm_node_info_s {
    dm_node_state_t state;
} dm_node_info_t;

/**
 * @brief Compares two data trees by module name
 */
static int
dm_data_info_cmp(const void *a, const void *b)
{
    assert(a);
    assert(b);
    dm_data_info_t *node_a = (dm_data_info_t *) a;
    dm_data_info_t *node_b = (dm_data_info_t *) b;

    int res = strcmp(node_a->module->name, node_b->module->name);
    if (res == 0) {
        return 0;
    } else if (res < 0) {
        return -1;
    } else {
        return 1;
    }
}

/**
 * @brief frees the dm_data_info stored in binary tree
 */
static void
dm_data_info_free(void *item)
{
    dm_data_info_t *info = (dm_data_info_t *) item;
    if (NULL != info) {
        lyd_free_withsiblings(info->node);
    }
    free(info);
}

/**
 * @brief Check whether the file_name corresponds to the schema file.
 * @return 1 if it does, 0 otherwise.
 */
static int
dm_is_schema_file(const char *file_name)
{
    CHECK_NULL_ARG(file_name);
    return sr_str_ends_with(file_name, SR_SCHEMA_YIN_FILE_EXT);
}

/**
 * @brief Loads the schema file into the context. The path for loading file is specified as concatenation of dir_name
 * and file_name. Function returns SR_ERR_OK if loading was successful. It might return SR_ERR_IO if the file can not
 * be opened, SR_ERR_INTERNAL if parsing of the file failed or SR_ERR_NOMEM if memory allocation failed.
 * @param [in] dm_ctx
 * @param [in] dir_name
 * @param [in] file_name
 * @return Error code (SR_ERR_OK on success)
 */
static int
dm_load_schema_file(dm_ctx_t *dm_ctx, const char *dir_name, const char *file_name)
{
    CHECK_NULL_ARG3(dm_ctx, dir_name, file_name);
    const struct lys_module *module = NULL;
    char *schema_filename = NULL;
    int rc = sr_str_join(dir_name, file_name, &schema_filename);
    if (SR_ERR_OK != rc) {
        return SR_ERR_NOMEM;
    }

    FILE *fd = fopen(schema_filename, "r");
    free(schema_filename);

    if (NULL == fd) {
        SR_LOG_WRN("Unable to open a schema file %s: %s", file_name, strerror(errno));
        return SR_ERR_IO;
    }

    pthread_rwlock_wrlock(&dm_ctx->lyctx_lock);
    module = lys_parse_fd(dm_ctx->ly_ctx, fileno(fd), LYS_IN_YIN);
    fclose(fd);
    if (module == NULL) {
        SR_LOG_WRN("Unable to parse a schema file: %s", file_name);
        pthread_rwlock_unlock(&dm_ctx->lyctx_lock);
        return SR_ERR_INTERNAL;
    }

    pthread_rwlock_unlock(&dm_ctx->lyctx_lock);
    return SR_ERR_OK;
}

/**
 * @brief Loops through the specified directory (dm_ctx->schema_search_dir) and tries to load schema files from it.
 * Schemas that can not be loaded are skipped.
 * @param [in] dm_ctx
 * @return Error code (SR_ERR_OK on success), SR_ERR_IO if the directory can not be opened
 */
static int
dm_load_schemas(dm_ctx_t *dm_ctx)
{
    CHECK_NULL_ARG(dm_ctx);
    DIR *dir = NULL;
    struct dirent *ent = NULL;
    if ((dir = opendir(dm_ctx->schema_search_dir)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (dm_is_schema_file(ent->d_name)) {
                if (SR_ERR_OK != dm_load_schema_file(dm_ctx, dm_ctx->schema_search_dir, ent->d_name)) {
                    SR_LOG_WRN("Loading schema file: %s failed.", ent->d_name);
                } else {
                    SR_LOG_INF("Schema file %s loaded successfully", ent->d_name);
                }
            }
        }
        closedir(dir);
        return SR_ERR_OK;
    } else {
        SR_LOG_ERR("Could not open the directory %s: %s", dm_ctx->schema_search_dir, strerror(errno));
        return SR_ERR_IO;
    }
}

/**
 * Checks whether the schema of the module has been loaded
 * @param [in] dm_ctx
 * @param [in] module_name
 * @return Error code (SR_ERR_OK on success), SR_ERR_UNKNOWN_MODEL
 */
static int
dm_find_module_schema(dm_ctx_t *dm_ctx, const char *module_name, const struct lys_module **module)
{
    CHECK_NULL_ARG2(dm_ctx, module_name);
    pthread_rwlock_rdlock(&dm_ctx->lyctx_lock);
    *module = ly_ctx_get_module(dm_ctx->ly_ctx, module_name, NULL);
    pthread_rwlock_unlock(&dm_ctx->lyctx_lock);
    return *module == NULL ? SR_ERR_UNKNOWN_MODEL : SR_ERR_OK;
}

/**
 * @brief Loads data tree from file. Module and datastore argument are used to
 * determine the file name.
 * @param [in] dm_ctx
 * @param [in] module
 * @param [in] ds
 * @param [out] data_info
 * @return Error code (SR_ERR_OK on success), SR_ERR_INTERAL if the parsing of the data tree fails.
 */
static int
dm_load_data_tree(dm_ctx_t *dm_ctx, const struct lys_module *module, sr_datastore_t ds, dm_data_info_t **data_info)
{
    CHECK_NULL_ARG2(dm_ctx, module);

    char *data_filename = NULL;
    int rc = 0;
    struct lyd_node *data_tree = NULL;
    *data_info = NULL;
    rc = sr_get_data_file_name(dm_ctx->data_search_dir, module->name, ds, &data_filename);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get data_filename failed for %s", module->name);
        return rc;
    }

    dm_data_info_t *data = NULL;
    data = calloc(1, sizeof(*data));
    if (NULL == data) {
        SR_LOG_ERR_MSG("Memory allocation failed");
        free(data_filename);
        return SR_ERR_NOMEM;
    }

    FILE *f = fopen(data_filename, "r");

    if (NULL != f) {
        lockf(fileno(f), F_LOCK, 0);
        struct stat st = {0};
        int rc = stat(data_filename, &st);
        if (-1 == rc) {
            SR_LOG_ERR_MSG("Stat failed");
            free(data_filename);
            free(data);
            fclose(f);
            return SR_ERR_INTERNAL;
        }
#ifdef HAVE_STAT_ST_MTIM
        data->timestamp = st.st_mtim;
        SR_LOG_DBG("Loaded module %s: mtime sec=%lld nsec=%lld\n", module->name,
                (long long) st.st_mtim.tv_sec,
                (long long) st.st_mtim.tv_nsec);
#else
        data->timestamp = st.st_mtime;
        SR_LOG_DBG("Loaded module %s: mtime sec=%lld\n", module->name, (long long) st.st_mtime);
#endif
        data_tree = lyd_parse_fd(dm_ctx->ly_ctx, fileno(f), LYD_XML, LYD_OPT_STRICT);
        lockf(fileno(f), F_ULOCK, 0);
        if (NULL == data_tree) {
            SR_LOG_ERR("Parsing data tree from file %s failed", data_filename);
            free(data_filename);
            free(data);
            fclose(f);
            return SR_ERR_INTERNAL;
        }
        fclose(f);
    } else {
        SR_LOG_INF("File %s couldn't be opened for reading: %s", data_filename, strerror(errno));
    }

    /* if the data tree is loaded, validate it*/
    if (NULL != data_tree && 0 != lyd_validate(data_tree, LYD_OPT_STRICT)) {
        SR_LOG_ERR("Loaded data tree '%s' is not valid", data_filename);
        free(data_filename);
        lyd_free_withsiblings(data_tree);
        free(data);
        return SR_ERR_INTERNAL;
    }

    data->module = module;
    data->modified = false;
    data->node = data_tree;

    if (NULL == data_tree) {
        SR_LOG_INF("Data file %s is empty", data_filename);
    } else {
        SR_LOG_INF("Data file %s loaded successfully", data_filename);
    }

    free(data_filename);

    *data_info = data;

    return SR_ERR_OK;
}

/**
 * @brief Fills the schema_t from lys_module structure
 * @return Error code (SR_ERR_OK on success), SR_ERR_INTERNAL in case of string duplication failure
 */
static int
dm_fill_schema_t(dm_ctx_t *dm_ctx, dm_session_t *session, const struct lys_module *module, sr_schema_t *schema)
{
    CHECK_NULL_ARG2(module, schema);
    CHECK_NULL_ARG3(module->name, module->prefix, module->ns);
    int rc = SR_ERR_INTERNAL;

    schema->module_name = strdup(module->name);
    schema->prefix = strdup(module->prefix);
    schema->ns = strdup(module->ns);
    if (NULL == schema->module_name || NULL == schema->prefix || NULL == schema->ns) {
        SR_LOG_ERR_MSG("Duplication of string for schema_t failed");
        goto cleanup;
    }

    /* revision is optional*/
    if (NULL != module->rev) {
        schema->revision = strdup(module->rev[0].date);
        if (NULL == schema->revision) {
            SR_LOG_ERR_MSG("Duplication of revision string failed");
            goto cleanup;
        }
    }

    rc = sr_get_schema_file_name(dm_ctx->schema_search_dir, module->name, &schema->file_path);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Get schema file name failed");
        goto cleanup;
    }
    return rc;

cleanup:
    free(schema->module_name);
    free(schema->prefix);
    free(schema->ns);
    free(schema->revision);
    free(schema->file_path);
    return rc;
}

static void
dm_free_lys_private_data(const struct lys_node *node, void *private)
{
    if (NULL != private) {
        free(private);
    }
}

static void
dm_free_sess_op(dm_sess_op_t *op)
{
    if (NULL == op) {
        return;
    }
    xp_free_loc_id(op->loc_id);
    sr_free_val(op->val);
}

static void
dm_free_sess_operations(dm_sess_op_t *ops, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        dm_free_sess_op(&ops[i]);
    }
    free(ops);
}

/**
 * @brief Returns the state of node. If the NULL is provided as an argument
 * DM_NODE_DISABLED is returned.
 */
static dm_node_state_t
dm_get_node_state(struct lys_node *node)
{
    if (NULL == node || NULL == node->private) {
        return DM_NODE_DISABLED;
    }
    dm_node_info_t *n_info = (dm_node_info_t *) node->private;

    if (NULL == n_info) {
        return DM_NODE_DISABLED;
    }
    return n_info->state;
}

int
dm_add_operation(dm_session_t *session, dm_operation_t op, xp_loc_id_t *loc_id, sr_val_t *val, sr_edit_options_t opts)
{
    int rc = SR_ERR_OK;
    CHECK_NULL_ARG_NORET2(rc, session, loc_id); /* value can be NULL*/
    if (SR_ERR_OK != rc) {
        goto cleanup;
    }

    if (NULL == session->operations){
        session->oper_size = 1;
        session->operations = calloc(session->oper_size, sizeof(*session->operations));
        if (NULL == session->operations){
            SR_LOG_ERR_MSG("Memory allocation failed");
            rc = SR_ERR_NOMEM;
            goto cleanup;
        }
    } else if (session->oper_count == session->oper_size){
        session->oper_size *= 2;
        dm_sess_op_t *tmp_op = realloc(session->operations, session->oper_size * sizeof(*session->operations));
        if (NULL == tmp_op){
            SR_LOG_ERR_MSG("Memory allocation failed");
            rc = SR_ERR_NOMEM;
            goto cleanup;
        }
        session->operations = tmp_op;
    }
    session->operations[session->oper_count].op = op;
    session->operations[session->oper_count].loc_id = loc_id;
    session->operations[session->oper_count].val = val;
    session->operations[session->oper_count].options = opts;

    session->oper_count++;
    return rc;
cleanup:
    xp_free_loc_id(loc_id);
    sr_free_val(val);
    return rc;
}

void
dm_clear_session_errors(dm_session_t *session)
{
    if (NULL == session) {
        return;
    }

    if (NULL != session->error_msg) {
        free(session->error_msg);
        session->error_msg = NULL;
    }

    if (NULL != session->error_xpath) {
        free(session->error_xpath);
        session->error_xpath = NULL;
    }
}

int
dm_report_error(dm_session_t *session, const char *msg, char *err_path, int rc)
{
    if (NULL == session) {
        return SR_ERR_INTERNAL;
    }

    /* if NULL is provided, message will be generated according to the error code*/
    if (NULL == msg) {
        msg = sr_strerror(rc);
    }

    if (NULL != session->error_msg) {
        SR_LOG_WRN("Overwriting session error message %s", session->error_msg);
        free(session->error_msg);
    }
    session->error_msg = strdup(msg);
    if (NULL == session->error_msg) {
        SR_LOG_ERR_MSG("Error message duplication failed");
        free(err_path);
        return SR_ERR_INTERNAL;
    }

    if (NULL != session->error_xpath) {
        SR_LOG_WRN("Overwriting session error xpath %s", session->error_xpath);
        free(session->error_xpath);
    }
    session->error_xpath = err_path;
    if (NULL == session->error_xpath) {
        SR_LOG_WRN_MSG("Error xpath passed to dm_report is NULL");
    }

    return rc;
}

bool
dm_has_error(dm_session_t *session)
{
    if (NULL == session) {
        return false;
    }
    return NULL != session->error_msg || NULL != session->error_xpath;
}

int
dm_copy_errors(dm_session_t *session, char **error_msg, char **err_xpath)
{
    CHECK_NULL_ARG3(session, error_msg, err_xpath);
    if (NULL != session->error_msg) {
        *error_msg = strdup(session->error_msg);
    }
    if (NULL != session->error_xpath) {
        *err_xpath = strdup(session->error_xpath);
    }
    if ((NULL != session->error_msg && NULL == *error_msg) || (NULL != session->error_xpath && NULL == *err_xpath)) {
        SR_LOG_ERR_MSG("Error duplication failed");
        return SR_ERR_INTERNAL;
    }
    return SR_ERR_OK;
}

bool
dm_is_node_enabled(struct lys_node* node)
{
    dm_node_state_t state = dm_get_node_state(node);
    return DM_NODE_ENABLED == state || DM_NODE_ENABLED_WITH_CHILDREN == state;
}

bool
dm_is_node_enabled_with_children(struct lys_node* node)
{
    return DM_NODE_ENABLED_WITH_CHILDREN == dm_get_node_state(node);
}

bool
dm_is_enabled_check_recursively(struct lys_node *node)
{
    if (dm_is_node_enabled(node)) {
        return true;
    }
    node = node->parent;
    while (NULL != node) {
        if (NULL == node->parent && LYS_AUGMENT == node->nodetype) {
            node = ((struct lys_node_augment *) node)->target;
            continue;
        }
        if (dm_is_node_enabled_with_children(node)) {
            return true;
        }
        node = node->parent;
    }
    return false;
}

int
dm_set_node_state(struct lys_node *node, dm_node_state_t state)
{
    CHECK_NULL_ARG(node);
    if (NULL == node->private) {
        node->private = calloc(1, sizeof(dm_node_info_t));
        if (NULL == node->private) {
            SR_LOG_ERR_MSG("Memory allocation failed");
            return SR_ERR_NOMEM;
        }
    }
    ((dm_node_info_t *) node->private)->state = state;
    return SR_ERR_OK;
}

bool
dm_is_running_ds_session(dm_session_t *session)
{
    if (NULL != session) {
        return SR_DS_RUNNING == session->datastore;
    }
    return false;
}

int
dm_init(const char *schema_search_dir, const char *data_search_dir, dm_ctx_t **dm_ctx)
{
    CHECK_NULL_ARG3(schema_search_dir, data_search_dir, dm_ctx);

    SR_LOG_INF("Initializing Data Manager, schema_search_dir=%s, data_search_dir=%s", schema_search_dir, data_search_dir);

    dm_ctx_t *ctx = NULL;
    ctx = calloc(1, sizeof(*ctx));
    if (NULL == ctx) {
        SR_LOG_ERR_MSG("Cannot allocate memory for Data Manager.");
        return SR_ERR_NOMEM;
    }
    ctx->ly_ctx = ly_ctx_new(schema_search_dir);
    if (NULL == ctx->ly_ctx) {
        SR_LOG_ERR_MSG("Cannot initialize libyang context in Data Manager.");
        dm_cleanup(ctx);
        return SR_ERR_NOMEM;
    }

    ctx->schema_search_dir = strdup(schema_search_dir);
    if (NULL == ctx->schema_search_dir) {
        SR_LOG_ERR_MSG("Cannot allocate memory for schema search dir string in Data Manager.");
        dm_cleanup(ctx);
        return SR_ERR_NOMEM;
    }

    ctx->data_search_dir = strdup(data_search_dir);
    if (NULL == ctx->data_search_dir) {
        SR_LOG_ERR_MSG("Cannot allocate memory for data search dir string in Data Manager.");
        dm_cleanup(ctx);
        return SR_ERR_NOMEM;
    }

    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
#if defined(HAVE_PTHREAD_RWLOCKATTR_SETKIND_NP)
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif

    int ret = pthread_rwlock_init(&ctx->lyctx_lock, &attr);
    pthread_rwlockattr_destroy(&attr);
    if (0 != ret) {
        SR_LOG_ERR_MSG("lyctx mutex initialization failed");
        dm_cleanup(ctx);
        return SR_ERR_INTERNAL;
    }

    *dm_ctx = ctx;
    int res = dm_load_schemas(ctx);
    if (SR_ERR_OK != res) {
        dm_cleanup(ctx);
        return res;
    }

    return SR_ERR_OK;
}

void
dm_cleanup(dm_ctx_t *dm_ctx)
{
    if (NULL != dm_ctx) {
        free(dm_ctx->schema_search_dir);
        free(dm_ctx->data_search_dir);
        if (NULL != dm_ctx->ly_ctx) {
            ly_ctx_destroy(dm_ctx->ly_ctx, dm_free_lys_private_data);
        }
        pthread_rwlock_destroy(&dm_ctx->lyctx_lock);
        free(dm_ctx);
    }
}

int
dm_session_start(const dm_ctx_t *dm_ctx, const sr_datastore_t ds, dm_session_t **dm_session_ctx)
{
    CHECK_NULL_ARG(dm_session_ctx);

    dm_session_t *session_ctx = NULL;
    session_ctx = calloc(1, sizeof(*session_ctx));
    if (NULL == session_ctx) {
        SR_LOG_ERR_MSG("Cannot allocate session_ctx in Data Manager.");
        return SR_ERR_NOMEM;
    }
    session_ctx->datastore = ds;

    int rc = SR_ERR_OK;
    rc = sr_btree_init(dm_data_info_cmp, dm_data_info_free, &session_ctx->session_modules);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Binary tree allocation failed");
        free(session_ctx);
        return SR_ERR_NOMEM;
    }
    *dm_session_ctx = session_ctx;

    return SR_ERR_OK;
}

int
dm_session_stop(const dm_ctx_t *dm_ctx, dm_session_t *session)
{
    CHECK_NULL_ARG2(dm_ctx, session);
    sr_btree_cleanup(session->session_modules);
    dm_clear_session_errors(session);
    dm_free_sess_operations(session->operations, session->oper_count);
    free(session);
    return SR_ERR_OK;
}

int
dm_get_data_info(dm_ctx_t *dm_ctx, dm_session_t *dm_session_ctx, const char *module_name, dm_data_info_t **info)
{
    CHECK_NULL_ARG4(dm_ctx, dm_session_ctx, module_name, info);
    int rc = SR_ERR_OK;
    const struct lys_module *module = NULL;
    dm_data_info_t *exisiting_data_info = NULL;

    if (dm_find_module_schema(dm_ctx, module_name, &module) != SR_ERR_OK) {
        SR_LOG_WRN("Unknown schema: %s", module_name);
        return SR_ERR_UNKNOWN_MODEL;
    }

    dm_data_info_t lookup_data = {0};
    lookup_data.module = module;
    exisiting_data_info = sr_btree_search(dm_session_ctx->session_modules, &lookup_data);

    if (NULL != exisiting_data_info) {
        *info = exisiting_data_info;
        SR_LOG_DBG("Module %s already loaded", module_name);
        return SR_ERR_OK;
    }

    /* session copy not found load it from file system */
    dm_data_info_t *di = NULL;
    rc = dm_load_data_tree(dm_ctx, module, dm_session_ctx->datastore, &di);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Getting data tree for %s failed.", module_name);
        return rc;
    }

    rc = sr_btree_insert(dm_session_ctx->session_modules, (void *) di);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Insert into session avl failed module %s", module_name);
        dm_data_info_free(di);
        return rc;
    }
    SR_LOG_DBG("Module %s has been loaded", module_name);
    *info = di;
    return SR_ERR_OK;
}

int
dm_get_datatree(dm_ctx_t *dm_ctx, dm_session_t *dm_session_ctx, const char *module_name, struct lyd_node **data_tree)
{
    CHECK_NULL_ARG4(dm_ctx, dm_session_ctx, module_name, data_tree);
    int rc = SR_ERR_OK;
    dm_data_info_t *info = NULL;
    rc = dm_get_data_info(dm_ctx, dm_session_ctx, module_name, &info);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get data info failed for module %s", module_name);
        return rc;
    }
    *data_tree = info->node;
    if (NULL == info->node) {
        return SR_ERR_NOT_FOUND;
    }
    return rc;
}

int
dm_get_module(dm_ctx_t *dm_ctx, const char *module_name, const char *revision, const struct lys_module **module)
{
    CHECK_NULL_ARG3(dm_ctx, module_name, module); /* revision might be NULL*/
    *module = ly_ctx_get_module(dm_ctx->ly_ctx, module_name, revision);
    if (NULL == *module) {
        SR_LOG_ERR("Get module failed %s", module_name);
        return SR_ERR_UNKNOWN_MODEL;
    }
    return SR_ERR_OK;
}

int
dm_list_schemas(dm_ctx_t *dm_ctx, dm_session_t *dm_session, sr_schema_t **schemas, size_t *schema_count)
{
    CHECK_NULL_ARG4(dm_ctx, dm_session, schemas, schema_count);
    size_t count = 0;
    size_t i = 0;
    sr_schema_t *sch = NULL;
    int rc = SR_ERR_OK;
    const char **names = ly_ctx_get_module_names(dm_ctx->ly_ctx);
    if (NULL == names) {
        *schema_count = 0;
        *schemas = NULL;
        return SR_ERR_OK;
    }

    while (NULL != names[count]) count++;

    sch = calloc(count, sizeof(*sch));
    if (NULL == sch) {
        SR_LOG_ERR_MSG("Memory allocation failed");
        free(names);
        return SR_ERR_NOMEM;
    }

    const struct lys_module *module = NULL;
    i = 0;
    while (NULL != names[i]) {
        module = ly_ctx_get_module(dm_ctx->ly_ctx, names[i], NULL);
        rc = dm_fill_schema_t(dm_ctx, dm_session, module, &sch[i]);
        if (SR_ERR_OK != rc) {
            SR_LOG_ERR_MSG("Filling sr_schema_t failed");
            sr_free_schemas(sch, i);
            free(names);
            return rc;
        }
        i++;
    }

    *schemas = sch;
    *schema_count = count;
    free(names);
    return SR_ERR_OK;
}

int
dm_validate_session_data_trees(dm_ctx_t *dm_ctx, dm_session_t *session, sr_error_info_t **errors, size_t *err_cnt)
{
    CHECK_NULL_ARG4(dm_ctx, session, errors, err_cnt);
    int rc = SR_ERR_OK;

    size_t cnt = 0;
    *err_cnt = 0;
    dm_data_info_t *info = NULL;
    while (NULL != (info = sr_btree_get_at(session->session_modules, cnt))) {
        /* loaded data trees are valid, so check only the modified ones */
        if (info->modified) {
            if (NULL == info->module || NULL == info->module->name) {
                SR_LOG_ERR_MSG("Missing schema information");
                sr_free_errors(*errors, *err_cnt);
                return SR_ERR_INTERNAL;
            }
            if (0 != lyd_validate(info->node, LYD_OPT_STRICT)) {
                SR_LOG_DBG("Validation failed for %s module", info->module->name);
                (*err_cnt)++;
                sr_error_info_t *tmp_err = realloc(*errors, *err_cnt * sizeof(**errors));
                if (NULL == tmp_err){
                    SR_LOG_ERR_MSG("Memory allocation failed");
                    sr_free_errors(*errors, *err_cnt - 1);
                    return SR_ERR_NOMEM;
                }
                *errors = tmp_err;
                (*errors)[(*err_cnt)-1].message = strdup(ly_errmsg());
                (*errors)[(*err_cnt)-1].path = strdup(ly_errpath());

                rc = SR_ERR_VALIDATION_FAILED;
            } else {
                SR_LOG_DBG("Validation succeeded for '%s' module", info->module->name);
            }
        }
        cnt++;
    }
    return rc;
}

int
dm_discard_changes(dm_ctx_t *dm_ctx, dm_session_t *session)
{
    CHECK_NULL_ARG2(dm_ctx, session);
    int rc = SR_ERR_OK;

    sr_btree_cleanup(session->session_modules);
    session->session_modules = NULL;

    rc = sr_btree_init(dm_data_info_cmp, dm_data_info_free, &session->session_modules);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Binary tree allocation failed");
        return SR_ERR_NOMEM;
    }

    return SR_ERR_OK;
}

int
dm_commit(dm_ctx_t *dm_ctx, dm_session_t *session, sr_error_info_t **errors, size_t *err_cnt)
{
    CHECK_NULL_ARG2(dm_ctx, session);
    int rc = SR_ERR_OK;
    //TODO send validate notifications

    /* YANG validation */
    rc = dm_validate_session_data_trees(dm_ctx, session, errors, err_cnt);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Data validation failed");
        return SR_ERR_COMMIT_FAILED;
    }

    /* TODO aquire data file lock*/
    /* TODO commit file lock to avoid deadlock when two commits - library, daemon mode*/

    /* lock context for writing */
    pthread_rwlock_wrlock(&dm_ctx->lyctx_lock);

    size_t cnt = 0;
    dm_data_info_t *info = NULL;
    while (NULL != (info = sr_btree_get_at(session->session_modules, cnt))) {
        if (info->modified) {
            char *data_filename = NULL;
            rc = sr_get_data_file_name(dm_ctx->data_search_dir, info->module->name, session->datastore, &data_filename);
            if (SR_ERR_OK != rc) {
                SR_LOG_ERR_MSG("Getting data file name failed");
                pthread_rwlock_unlock(&dm_ctx->lyctx_lock);
                return rc;
            }
            struct stat st = {0};
            int rc = stat(data_filename, &st);
            if (-1 == rc) {
                SR_LOG_ERR_MSG("Stat failed");
                free(data_filename);
                pthread_rwlock_unlock(&dm_ctx->lyctx_lock);
                return SR_ERR_INTERNAL;
            }
            FILE *f = fopen(data_filename, "w");
            if (NULL == f) {
                SR_LOG_ERR("Failed to open file %s", data_filename);
                pthread_rwlock_unlock(&dm_ctx->lyctx_lock);
                free(data_filename);
                return SR_ERR_IO;
            }
            lockf(fileno(f), F_LOCK, 0);


#ifdef HAVE_STAT_ST_MTIM
            if ((info->timestamp.tv_sec != st.st_mtim.tv_sec)
                    || (info->timestamp.tv_nsec != st.st_mtim.tv_nsec)) {
                SR_LOG_INF("Merging needs to be done for module '%s', currently just overwriting", info->module->name);
            } else {
                SR_LOG_INF("Session copy module '%s', has not been changed since loading", info->module->name);
            }
#else
            if (info->timestamp != st.st_mtime) {
                SR_LOG_INF("Merging needs to be done for module '%s', currently just overwriting", info->module->name);
            } else {
                /* Further check if the because we have only second precision */
            }
#endif
            if (0 != lyd_print_file(f, info->node, LYD_XML_FORMAT, LYP_WITHSIBLINGS)) {
                SR_LOG_ERR("Failed to write output into %s", data_filename);
                pthread_rwlock_unlock(&dm_ctx->lyctx_lock);
                free(data_filename);
                return SR_ERR_INTERNAL;
            }
            lockf(fileno(f), F_ULOCK, 0);
            fclose(f);

            free(data_filename);
            if (SR_ERR_OK != rc) {
                SR_LOG_ERR("Saving data file for module %s failed", info->module->name);
                pthread_rwlock_unlock(&dm_ctx->lyctx_lock);
                return rc;
            }
        }
        cnt++;
    }

    pthread_rwlock_unlock(&dm_ctx->lyctx_lock);

    /* discard changes in session in next get_data_tree call newly committed content will be loaded */
    rc = dm_discard_changes(dm_ctx, session);

    return rc;
}
