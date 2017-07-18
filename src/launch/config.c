/*
 * D-Bus XML Config Parser
 */

#include <c-list.h>
#include <c-macro.h>
#include <expat.h>
#include <stdlib.h>
#include "launch/config.h"
#include "util/error.h"

static_assert(__builtin_types_compatible_p(XML_Char, char),
              "Missing UTF-8 support in expat");

/* Print an error message to stderr including file+line information */
#define CONFIG_ERR(_state, _intro, _outro, ...)                                 \
        fprintf(stderr,                                                         \
                _intro " in %s +%lu" _outro "\n",                               \
                (_state)->file->path,                                           \
                XML_GetCurrentLineNumber(                                       \
                        c_container_of((_state), ConfigParser, state)->xml),    \
                ## __VA_ARGS__)

/**
 * config_path_new() - XXX
 */
int config_path_new(ConfigPath **filep, ConfigPath *parent, const char *path) {
        _c_cleanup_(config_path_unrefp) ConfigPath *file = NULL;
        size_t n_path, n_prefix;
        char *t, *prefix = NULL;

        n_path = strlen(path);
        n_prefix = 0;

        /* prepend parent-path if @path is relative */
        if (parent && path[0] != '/') {
                if (parent->is_dir) {
                        prefix = parent->path;
                        n_prefix = strlen(parent->path);
                } else {
                        t = strrchr(parent->path, '/');
                        if (t) {
                                prefix = parent->path;
                                n_prefix = t - parent->path;
                        }
                }
        }

        file = calloc(1, sizeof(*file) + n_prefix + 1 + n_path + 1);
        if (!file)
                return error_origin(-ENOMEM);

        *file = (ConfigPath)CONFIG_FILE_NULL(*file);
        file->parent = config_path_ref(parent);

        if (n_prefix) {
                memcpy(file->path, prefix, n_prefix);
                file->path[n_prefix] = '/';
                memcpy(file->path + n_prefix + 1, path, n_path + 1);
        } else {
                memcpy(file->path, path, n_path + 1);
        }

        *filep = file;
        file = NULL;
        return 0;
}

static int config_path_new_dir(ConfigPath **filep, ConfigPath *parent, const char *path) {
        int r;

        r = config_path_new(filep, parent, path);
        if (!r)
                (*filep)->is_dir = true;

        return r;
}

/**
 * config_path_ref() - XXX
 */
ConfigPath *config_path_ref(ConfigPath *file) {
        if (file)
                ++file->n_refs;
        return file;
}

/**
 * config_path_unref() - XXX
 */
ConfigPath *config_path_unref(ConfigPath *file) {
        ConfigPath *parent;

        if (!file || --file->n_refs)
                return NULL;

        parent = file->parent;
        free(file);

        return config_path_unref(parent);
}

/**
 * config_node_new() - XXX
 */
int config_node_new(ConfigNode **nodep, ConfigNode *parent, unsigned int type) {
        _c_cleanup_(config_node_freep) ConfigNode *node = NULL;

        node = calloc(1, sizeof(*node));
        if (!node)
                return error_origin(-ENOMEM);

        *node = (ConfigNode)CONFIG_NODE_NULL(*node);
        node->parent = parent;
        if (parent)
                ++parent->n_children;
        node->type = type;

        *nodep = node;
        node = NULL;
        return 0;
}

/**
 * config_node_free() - XXX
 */
ConfigNode *config_node_free(ConfigNode *node) {
        if (!node)
                return NULL;

        switch (node->type) {
        case CONFIG_NODE_INCLUDEDIR:
                config_path_unref(node->includedir.dir);
                break;
        case CONFIG_NODE_INCLUDE:
                config_path_unref(node->include.file);
                break;
        }

        free(node->cdata);
        assert(!node->n_children);

        if (node->parent)
                --node->parent->n_children;

        c_list_unlink_init(&node->include_link);
        c_list_unlink_init(&node->root_link);
        free(node);

        return NULL;
}

/**
 * config_root_new() - XXX
 */
int config_root_new(ConfigRoot **rootp) {
        _c_cleanup_(config_root_freep) ConfigRoot *root = NULL;

        root = calloc(1, sizeof(*root));
        if (!root)
                return error_origin(-ENOMEM);

        *root = (ConfigRoot)CONFIG_ROOT_NULL(*root);

        *rootp = root;
        root = NULL;
        return 0;
}

/**
 * config_root_free() - XXX
 */
ConfigRoot *config_root_free(ConfigRoot *root) {
        ConfigNode *i_node;

        if (!root)
                return NULL;

        while ((i_node = c_list_last_entry(&root->node_list, ConfigNode, root_link)))
                config_node_free(i_node);

        assert(c_list_is_empty(&root->node_list));
        assert(c_list_is_empty(&root->include_list));

        free(root);

        return NULL;
}

static int config_parser_attrs_include(ConfigState *state, ConfigNode *node, const XML_Char **attrs) {
        const char *k, *v;

        while (*attrs) {
                k = *(attrs++);
                v = *(attrs++);

                if (!strcmp(k, "ignore_missing")) {
                        if (!strcmp(v, "yes"))
                                node->include.ignore_missing = true;
                        else if (!strcmp(v, "no"))
                                node->include.ignore_missing = false;
                        else
                                CONFIG_ERR(state, "Invalid value", ": %s=\"%s\"", k, v);
                } else if (!strcmp(k, "if_selinux_enabled")) {
                        if (!strcmp(v, "yes"))
                                node->include.if_selinux_enabled = true;
                        else if (!strcmp(v, "no"))
                                node->include.if_selinux_enabled = false;
                        else
                                CONFIG_ERR(state, "Invalid value", ": %s=\"%s\"", k, v);
                } else if (!strcmp(k, "selinux_root_relative")) {
                        if (!strcmp(v, "yes"))
                                node->include.selinux_root_relative = true;
                        else if (!strcmp(v, "no"))
                                node->include.selinux_root_relative = false;
                        else
                                CONFIG_ERR(state, "Invalid value", ": %s=\"%s\"", k, v);
                } else {
                        CONFIG_ERR(state, "Unknown attribute", ": %s=\"%s\"", k, v);
                }
        }

        return 0;
}

static int config_parser_attrs_policy(ConfigState *state, ConfigNode *node, const XML_Char **attrs) {
        /*
         * XXX: Attributes to support:
         *       * context (default|mandatory) #IMPLIED
         *       * user CDATA #IMPLIED
         *       * group CDATA #IMPLIED
         *       * at_console (yes|no) #IMPLIED
         */
        return 0;
}

static int config_parser_attrs_limit(ConfigState *state, ConfigNode *node, const XML_Char **attrs) {
        /*
         * XXX: Attributes to support:
         *       * name CDATA #REQUIRED
         */
        return 0;
}

static int config_parser_attrs_apparmor(ConfigState *state, ConfigNode *node, const XML_Char **attrs) {
        /*
         * XXX: Attributes to support:
         *       * mode (required|enabled|disabled) "enabled"
         */
        return 0;
}

static int config_parser_attrs_allow(ConfigState *state, ConfigNode *node, const XML_Char **attrs) {
        /*
         * XXX: Attributes to support:
         *       * user CDATA #IMPLIED
         *       * send CDATA #IMPLIED
         *       * receive CDATA #IMPLIED
         *       * own CDATA #IMPLIED
         *       * send_to CDATA #IMPLIED
         *       * receive_from CDATA #IMPLIED
         */
        return 0;
}

static int config_parser_attrs_deny(ConfigState *state, ConfigNode *node, const XML_Char **attrs) {
        /*
         * XXX: Attributes to support:
         *       * user CDATA #IMPLIED
         *       * send CDATA #IMPLIED
         *       * receive CDATA #IMPLIED
         *       * own CDATA #IMPLIED
         *       * send_to CDATA #IMPLIED
         *       * receive_from CDATA #IMPLIED
         */
        return 0;
}

static int config_parser_attrs_associate(ConfigState *state, ConfigNode *node, const XML_Char **attrs) {
        /*
         * XXX: Attributes to support:
         *       * own CDATA #REQUIRED
         *       * context CDATA #REQUIRED
         */
        return 0;
}

static int config_parser_attrs_default(ConfigState *state, ConfigNode *node, const XML_Char **attrs) {
        const char *k, *v;

        while (*attrs) {
                k = *(attrs++);
                v = *(attrs++);

                CONFIG_ERR(state, "Unknown attribute", ": %s=\"%s\"", k, v);
        }

        return 0;
}

static void config_parser_begin_fn(void *userdata, const XML_Char *name, const XML_Char **attrs) {
        _c_cleanup_(config_node_freep) ConfigNode *node = NULL;
        ConfigState *state = userdata;
        int r = 0;

        assert(state->current);

        /*
         * Whenever we hit a fatal error, we remember it in @state and simply
         * shortcut everything. However, when we just hit unknown tags, we
         * track our nesting depth and ignore anything underneath that tag, but
         * continue parsing once we are back at a known state.
         */
        if (state->error)
                return;
        if (state->n_failed) {
                ++state->n_failed;
                return;
        }

        if (!strcmp(name, "busconfig")) {

                if (state->n_depth)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_BUSCONFIG);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "user")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_USER);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "type")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_TYPE);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "fork")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_FORK);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "keep_umask")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_KEEP_UMASK);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "listen")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_LISTEN);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "pidfile")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_PIDFILE);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "includedir")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_INCLUDEDIR);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "servicedir")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_SERVICEDIR);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "servicehelper")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_SERVICEHELPER);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "auth")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_AUTH);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "include")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_INCLUDE);
                if (r)
                        goto failed;

                r = config_parser_attrs_include(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "policy")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_POLICY);
                if (r)
                        goto failed;

                r = config_parser_attrs_policy(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "limit")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_LIMIT);
                if (r)
                        goto failed;

                r = config_parser_attrs_limit(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "selinux")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_SELINUX);
                if (r)
                        goto failed;

                r = config_parser_attrs_default(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "apparmor")) {

                if (state->current->type != CONFIG_NODE_BUSCONFIG)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_APPARMOR);
                if (r)
                        goto failed;

                r = config_parser_attrs_apparmor(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "allow")) {

                if (state->current->type != CONFIG_NODE_POLICY)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_ALLOW);
                if (r)
                        goto failed;

                r = config_parser_attrs_allow(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "deny")) {

                if (state->current->type != CONFIG_NODE_POLICY)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_DENY);
                if (r)
                        goto failed;

                r = config_parser_attrs_deny(state, node, attrs);
                if (r)
                        goto failed;

        } else if (!strcmp(name, "associate")) {

                if (state->current->type != CONFIG_NODE_SELINUX)
                        goto failed;

                r = config_node_new(&node, state->current, CONFIG_NODE_ASSOCIATE);
                if (r)
                        goto failed;

                r = config_parser_attrs_associate(state, node, attrs);
                if (r)
                        goto failed;

        } else {
                goto failed;
        }

        assert(node);
        assert(node->parent);

        c_list_link_after(&state->last->root_link, &node->root_link);
        state->current = node;
        state->last = node;
        ++state->n_depth;
        node = NULL;
        return;

failed:
        if (r)
                state->error = error_trace(r);
        else
                ++state->n_failed;
}

static void config_parser_end_fn(void *userdata, const XML_Char *name) {
        ConfigState *state = userdata;
        int r;

        /*
         * Shortcut on errors. This is orthogonal to the begin-handler. See
         * there for details.
         */
        if (state->error)
                return;
        if (state->n_failed) {
                --state->n_failed;
                return;
        }

        /*
         * Before exiting a node, we verify that we got a valid dataset and
         * that all mandatory data was given.
         */
        switch (state->current->type) {
        case CONFIG_NODE_INCLUDEDIR: {
                _c_cleanup_(c_closedirp) DIR *dir = NULL;
                static const char suffix[] = ".conf";
                struct dirent *de;
                size_t n;

                r = config_path_new_dir(&state->current->includedir.dir,
                                        state->file,
                                        state->current->cdata);
                if (r) {
                        state->error = error_trace(r);
                        return;
                }

                dir = opendir(state->current->includedir.dir->path);
                if (!dir) {
                        if (errno == ENOENT || errno == ENOTDIR)
                                break;

                        state->error = error_origin(-errno);
                        return;
                }

                for (errno = 0, de = readdir(dir);
                     de;
                     errno = 0, de = readdir(dir)) {
                        _c_cleanup_(config_node_freep) ConfigNode *node = NULL;

                        n = strlen(de->d_name);

                        if (n <= strlen(suffix))
                                continue;
                        if (strcmp(de->d_name + n - strlen(suffix), suffix))
                                continue;

                        r = config_node_new(&node, state->current, CONFIG_NODE_INCLUDE);
                        if (r) {
                                state->error = error_trace(r);
                                return;
                        }

                        r = config_path_new(&node->include.file,
                                            state->current->includedir.dir,
                                            de->d_name);
                        if (r) {
                                state->error = error_trace(r);
                                return;
                        }

                        c_list_link_after(&state->last->root_link, &node->root_link);
                        c_list_link_tail(&state->root->include_list, &node->include_link);
                        state->last = node;
                        node = NULL;
                }

                break;
        }

        case CONFIG_NODE_INCLUDE: {
                r = config_path_new(&state->current->include.file,
                                    state->file,
                                    state->current->cdata);
                if (r) {
                        state->error = error_trace(r);
                        return;
                }

                c_list_link_tail(&state->root->include_list, &state->current->include_link);
                break;
        }

        case CONFIG_NODE_USER:
        case CONFIG_NODE_TYPE:
        case CONFIG_NODE_LISTEN:
        case CONFIG_NODE_PIDFILE:
        case CONFIG_NODE_SERVICEDIR:
        case CONFIG_NODE_SERVICEHELPER:
        case CONFIG_NODE_AUTH:
        case CONFIG_NODE_LIMIT:
                /* XXX: Not yet implemented. */
                break;

        case CONFIG_NODE_BUSCONFIG:
        case CONFIG_NODE_FORK:
        case CONFIG_NODE_KEEP_UMASK:
        case CONFIG_NODE_POLICY:
        case CONFIG_NODE_SELINUX:
        case CONFIG_NODE_APPARMOR:
        case CONFIG_NODE_ALLOW:
        case CONFIG_NODE_DENY:
        case CONFIG_NODE_ASSOCIATE:
                /* fallthrough */
        default:
                if (state->current->cdata &&
                    strspn(state->current->cdata, " \r\t\n") < state->current->n_cdata)
                        CONFIG_ERR(state, "Unknown character value", ": <%s>#CDATA</%s>", name, name);

                break;
        }

        /*
         * Verify our state is correct and then traverse one level up the tree.
         * That is, set the parent as the new current node and decrement the
         * depth.
         */
        assert(state->n_depth);
        assert(state->current);
        assert(state->current->parent);

        --state->n_depth;
        state->current = state->current->parent;
}

static void config_parser_blob_fn(void *userdata, const XML_Char *data, int n_data) {
        ConfigState *state = userdata;
        char *t;

        /* Shortcut on errors. Simply bail out and skip the handler. */
        if (state->error || state->n_failed)
                return;

        /* Append @data to existing @cdata */
        t = malloc(state->current->n_cdata + n_data + 1);
        if (!t) {
                state->error = error_origin(-ENOMEM);
                return;
        }

        memcpy(t, state->current->cdata, state->current->n_cdata);
        memcpy(t + state->current->n_cdata, data, n_data);
        t[state->current->n_cdata + n_data] = 0;
        free(state->current->cdata);
        state->current->cdata = t;
        state->current->n_cdata += n_data;
}

/**
 * config_parser_init() - XXX
 */
void config_parser_init(ConfigParser *parser) {
        *parser = (ConfigParser)CONFIG_PARSER_NULL(*parser);

        parser->xml = XML_ParserCreate(NULL);
}

/**
 * config_parser_deinit() - XXX
 */
void config_parser_deinit(ConfigParser *parser) {
        if (parser->xml)
                XML_ParserFree(parser->xml);

        *parser = (ConfigParser)CONFIG_PARSER_NULL(*parser);
}

static int config_parser_include(ConfigParser *parser, ConfigRoot *root, ConfigNode *node) {
        _c_cleanup_(c_closep) int fd = -1;
        char buffer[CONFIG_PARSER_BUFFER_MAX];
        ConfigPath *i_file;
        ssize_t len;
        int r;

        assert(node->type == CONFIG_NODE_INCLUDE);
        assert(node->include.file);

        memset(&parser->state, 0, sizeof(parser->state));
        parser->state.file = node->include.file;
        parser->state.root = root;
        parser->state.current = node;
        parser->state.last = node;

        /* ignore selinux files (XXX: fix when selinux is supported) */
        if (node->include.if_selinux_enabled)
                return 0;

        /* ignore recursive inclusions */
        for (i_file = node->include.file->parent; i_file; i_file = i_file->parent) {
                if (!strcmp(node->include.file->path, i_file->path)) {
                        CONFIG_ERR(&parser->state, "Recursive inclusion", "");
                        return 0;
                }
        }

        XML_ParserReset(parser->xml, NULL);
        XML_SetUserData(parser->xml, &parser->state);
        XML_SetElementHandler(parser->xml, config_parser_begin_fn, config_parser_end_fn);
        XML_SetCharacterDataHandler(parser->xml, config_parser_blob_fn);

        r = open(node->include.file->path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (r < 0) {
                if (errno == ENOENT || errno == ENOTDIR)
                        return node->include.ignore_missing ? 0 : error_origin(-errno);

                return error_origin(-errno);
        }
        fd = r;

        do {
                len = read(fd, buffer, sizeof(buffer));
                if (len < 0)
                        return error_origin(-errno);

                r = XML_Parse(parser->xml, buffer, len, len ? XML_FALSE : XML_TRUE);
                if (r != XML_STATUS_OK) {
                        CONFIG_ERR(&parser->state, "Invalid XML", ": %s",
                                   XML_ErrorString(XML_GetErrorCode(parser->xml)));
                        return CONFIG_E_INVALID;
                }

                if (parser->state.error)
                        return error_trace(parser->state.error);
        } while (len);

        assert(!parser->state.n_depth);
        assert(!parser->state.n_failed);

        return 0;
}

/**
 * config_parser_read() - XXX
 */
int config_parser_read(ConfigParser *parser, ConfigRoot **rootp, const char *path) {
        _c_cleanup_(config_root_freep) ConfigRoot *root = NULL;
        _c_cleanup_(config_path_unrefp) ConfigPath *file = NULL;
        ConfigNode *node;
        int r;

        /*
         * First create a fresh root config entry which we will use to link all
         * parsed nodes to. This is what is returned in the end to the caller.
         */
        r = config_root_new(&root);
        if (r)
                return error_trace(r);

        /*
         * Create a fake <include>@path</include> node on the root entry, which
         * serves as starting point.
         */
        r = config_path_new(&file, NULL, path);
        if (r)
                return error_trace(r);

        r = config_node_new(&node, NULL, CONFIG_NODE_INCLUDE);
        if (r)
                return error_trace(r);

        node->include.file = config_path_ref(file);
        c_list_link_front(&root->node_list, &node->root_link);
        c_list_link_front(&root->include_list, &node->include_link);

        /*
         * Now for as long as we find include-nodes linked on
         * @root->include_list, we call into config_parser_include(). This will
         * fill in all the contents of the include.
         */
        while ((node = c_list_first_entry(&root->include_list, ConfigNode, include_link))) {
                c_list_unlink_init(&node->include_link);

                r = config_parser_include(parser, root, node);
                if (r)
                        return error_trace(r);
        }

        *rootp = root;
        root = NULL;
        return 0;
}