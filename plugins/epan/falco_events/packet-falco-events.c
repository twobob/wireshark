/* packet-falco-events.c
 *
 * By Loris Degioanni
 * Copyright (C) 2021 Sysdig, Inc.
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// To do:
// - Convert this to C++? It would let us get rid of the glue that is
//   sinsp-span and make string handling a lot easier. However,
//   epan/address.h and driver/ppm_events_public.h both define PT_NONE.
// - Add a configuration preference for configure_plugin?
// - Add a configuration preference for individual conversation filters vs ANDing them?
//   We would need to add deregister_(|log_)conversation_filter before we implement this.
// - Add syscall IP address conversation support
// - Add prefs for
//   - set_snaplen
//   - set_dopfailed
//   - set_import_users

#include "config.h"
#define WS_LOG_DOMAIN "falco-events"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef _WIN32
#include <unistd.h>
#include <dlfcn.h>
#endif

#include <wiretap/wtap.h>

#include <epan/conversation.h>
#include <epan/conversation_filter.h>
#include <epan/dfilter/dfilter-translator.h>
#include <epan/dfilter/sttype-field.h>
#include <epan/dfilter/sttype-op.h>
#include <epan/exceptions.h>
#include <epan/follow.h>
#include <epan/maxmind_db.h>
#include <epan/packet.h>
#include <epan/prefs.h>
#include <epan/proto.h>
#include <epan/proto_data.h>
#include <epan/stats_tree.h>
#include <epan/stat_tap_ui.h>
#include <epan/tap.h>

#include <epan/dissectors/packet-sysdig-event.h>

#include <wsutil/file_util.h>
#include <wsutil/filesystem.h>
#include <wsutil/inet_addr.h>
#include <wsutil/report_message.h>
#include <wsutil/strtoi.h>

#include "sinsp-span.h"

#define FALCO_PPME_PLUGINEVENT_E 322
#define DEFAULT_CONTAINER_ID "host"

typedef enum bridge_field_flags_e {
    BFF_NONE = 0,
    BFF_HIDDEN = 1 << 1, // Unused
    BFF_INFO = 1 << 2,
    BFF_CONVERSATION = 1 << 3
} bridge_field_flags_e;

typedef struct conv_filter_info {
    hf_register_info *field_info;
    bool is_present;
    wmem_strbuf_t *strbuf;
} conv_filter_info;

typedef enum geoip_off_e {
    GEOIP_SUMMARY,
    GEOIP_COUNTRY,
    GEOIP_COUNTRY_ISO,
    GEOIP_CITY,
    GEOIP_AS_NUMBER,
    GEOIP_AS_ORG,
    GEOIP_LATITUDE,
    GEOIP_LONGITUDE,
    GEOIP_FIELD_COUNT
} geoip_off_e;

typedef struct bridge_info {
    sinsp_source_info_t *ssi;
    uint32_t source_id;
    data_source_media_type_e media_type;
    int proto;
    hf_register_info* hf;
    int* hf_ids;
    hf_register_info* hf_v4;
    int *hf_v4_ids;
    hf_register_info* hf_v6;
    int *hf_v6_ids;
    hf_register_info* hf_geoip;
    int *hf_geoip_ids;
    int* hf_id_to_addr_id; // Maps an hf offset to an hf_v[46] offset
    unsigned visible_fields;
    unsigned addr_fields;
    uint32_t* field_flags;
    int* field_ids;
    uint32_t num_conversation_filters;
    conv_filter_info *conversation_filters;
} bridge_info;

typedef struct proto_syscall_layer_data {
    int proto_hfi;
    int offset;
    int length;
    proto_tree *proto_ti;
} proto_syscall_layer_data;

typedef struct falco_conv_filter_fields {
    const char* container_id;
    int64_t pid;
    int64_t tid;
    int64_t fd;
    const char* fd_containername;
} falco_conv_filter_fields;

typedef struct fd_follow_tap_info {
    const char* data;
    int32_t datalen;
    bool is_write;
} fd_follow_tap_info;

typedef struct container_io_tap_info {
    const char* container_id;
    const char* proc_name;
    const char* fd_name;
    int32_t io_bytes;
    bool is_write;
} container_io_tap_info;

// This exists in case we want to add any statistics. Otherwise
// we can just cast the index as a pointer.
typedef struct fd_stream_info {
    uint32_t stream_index;
} fd_stream_info;

static dissector_handle_t syscall_evt_dissector_handle;
static dissector_handle_t syscall_fd_dissector_handle;

static int proto_falco_events;
static int proto_falco_json_events;
static int proto_syscalls[NUM_SINSP_SYSCALL_CATEGORIES];

static int ett_falco_events;
static int ett_syscalls[NUM_SINSP_SYSCALL_CATEGORIES];
static int ett_lineage[N_PROC_LINEAGE_ENTRIES];

static int ett_sinsp_enriched;
static int ett_sinsp_span;
static int ett_address;
static int ett_json;
static int ett_geoip_info;

static int container_io_tap;

static bool pref_show_internal;

static dissector_table_t ptype_dissector_table;
static dissector_handle_t json_handle;

static int fd_follow_tap;
static uint32_t fd_stream_count;

static int dissect_sinsp_enriched(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *bi_ptr, sysdig_event_param_data *event_param_data);
static int dissect_sinsp_plugin(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *bi_ptr);
static bridge_info* get_bridge_info(uint32_t source_id);
const char* get_str_value(sinsp_field_extract_t *sinsp_fields, uint32_t sf_idx);

/*
 * Array of plugin bridges
 */
bridge_info* bridges;
size_t nbridges;

/*
 * sinsp extractor span
 */
sinsp_span_t *sinsp_span;

/*
 * Fields
 */

// We originally showed the libscap event parameters with each plugin event.
// It's not clear if they're useful, so comment them out for now. If there
// are no complaints, this code should be removed.
// static int hf_sdp_source_id_size;
// static int hf_sdp_lengths;
static int hf_sdp_source_id;
static int hf_fd_stream;

static hf_register_info hf[] = {
    // { &hf_sdp_source_id_size,
    //     { "Plugin ID size", "falcoevents.id.size",
    //     FT_UINT32, BASE_DEC,
    //     NULL, 0x0,
    //     NULL, HFILL }
    // },
    // { &hf_sdp_lengths,
    //     { "Field Lengths", "falcoevents.lens",
    //     FT_UINT32, BASE_DEC,
    //     NULL, 0x0,
    //     NULL, HFILL }
    // },
    { &hf_sdp_source_id,
        { "Plugin ID", "falcoevents.id",
        FT_UINT32, BASE_DEC,
        NULL, 0x0,
        NULL, HFILL }
    },
    { &hf_fd_stream,
        { "Stream index", "falcoevents.fd.stream",
         FT_UINT32, BASE_DEC,
         NULL, 0x0,
         NULL, HFILL }
    },
};

static void
falco_events_cleanup(void) {
    close_sinsp_capture(sinsp_span);
}

// Returns true if the field should be used for conversation filters.
// XXX This should probably be a preference.
static bool
is_conversation_field(enum ftenum ftype, const char *abbrev) {
    if (ftype != FT_STRINGZ) {
        return false;
    }

    if (strcmp(abbrev, "ct.shortsrc") == 0) {
        return true;
    } else if (strstr(abbrev, "ct.user.accountid")) {
        return true;
    }
    return false;
}

// Returns true if the field might contain an IPv4 or IPv6 address.
// XXX This should probably be a preference.
static bool
is_source_address_field(enum ftenum ftype, const char *abbrev) {
    if (ftype != FT_STRINGZ) {
        return false;
    }

    const char *addr_suffixes[] = {
        ".srcip",       // ct.srcip
        ".callerIP",    // gcp.callerIP
        ".client.ip",   // okta.client.ip
    };
    for (size_t idx = 0; idx < array_length(addr_suffixes); idx++) {
        if (g_str_has_suffix(abbrev, addr_suffixes[idx])) {
            return true;
        }
    }
    return false;
}

static bool
is_filter_valid(packet_info *pinfo, void *cfi_ptr)
{
    conv_filter_info *cfi = (conv_filter_info *)cfi_ptr;

    if (!cfi->is_present) {
        return false;
    }

    int proto_id = proto_registrar_get_parent(cfi->field_info->hfinfo.id);

    if (proto_id < 0) {
        return false;
    }

    return proto_is_frame_protocol(pinfo->layers, proto_registrar_get_nth(proto_id)->abbrev);
}

static char*
build_conversation_filter(packet_info *pinfo _U_, void *cfi_ptr)
{
    conv_filter_info *cfi = (conv_filter_info *)cfi_ptr;

    if (!cfi->is_present) {
        return NULL;
    }

    return ws_strdup_printf("%s eq %s", cfi->field_info->hfinfo.abbrev, cfi->strbuf->str);
}

// Falco rule translation

const char *
stnode_op_to_string(stnode_op_t op) {
    switch (op) {
    case STNODE_OP_NOT:         return "!";
    case STNODE_OP_AND:         return "and";
    case STNODE_OP_OR:          return "or";
    case STNODE_OP_ANY_EQ:      return "=";
    case STNODE_OP_ALL_NE:      return "!=";
    case STNODE_OP_GT:          return ">";
    case STNODE_OP_GE:          return ">=";
    case STNODE_OP_LT:          return "<";
    case STNODE_OP_LE:          return "<=";
    case STNODE_OP_CONTAINS:    return "icontains";
    case STNODE_OP_UNARY_MINUS: return "-";
    case STNODE_OP_IN:
    case STNODE_OP_NOT_IN:
    default:
        break;
    }
    return NULL;
}

char *hfinfo_to_filtercheck(header_field_info *hfinfo) {
    if (!hfinfo) {
        return NULL;
    }

    const char *filtercheck = NULL;
    for (size_t br_idx = 0; br_idx < nbridges && !filtercheck; br_idx++) {
        bridge_info *bridge = &bridges[br_idx];
        unsigned hf_idx;
        for (hf_idx = 0; hf_idx < bridge->visible_fields; hf_idx++) {
            if (hfinfo->id == bridge->hf_ids[hf_idx]) {
                ptrdiff_t pfx_off = 0;
                if (g_str_has_prefix(hfinfo->abbrev, FALCO_FIELD_NAME_PREFIX)) {
                    pfx_off = strlen(FALCO_FIELD_NAME_PREFIX);
                }
                return g_strdup(hfinfo->abbrev + pfx_off);
            }
        }
        for (hf_idx = 0; hf_idx < bridge->addr_fields; hf_idx++) {
            if (hfinfo->id == bridge->hf_v4_ids[hf_idx] || hfinfo->id == bridge->hf_v6_ids[hf_idx]) {
                size_t fc_len = strlen(hfinfo->abbrev) - strlen(".v?");
                return g_strndup(hfinfo->abbrev, fc_len);
            }
        }
    }
    return NULL;
}

// Falco rule syntax is specified at
// https://github.com/falcosecurity/libs/blob/master/userspace/libsinsp/filter/parser.h

// NOLINTNEXTLINE(misc-no-recursion)
bool visit_dfilter_node(stnode_t *node, stnode_op_t parent_bool_op, GString *falco_rule)
{
    stnode_t *left, *right;

    if (stnode_type_id(node) == STTYPE_TEST) {
        stnode_op_t op = STNODE_OP_UNINITIALIZED;
        sttype_oper_get(node, &op, &left, &right);

        const char *op_str = stnode_op_to_string(op);
        if (!op_str) {
            return false;
        }

        if (left && right) {
            if ((op == STNODE_OP_ANY_EQ || op == STNODE_OP_ALL_NE) && stnode_type_id(right) != STTYPE_FVALUE) {
                // XXX Not yet supported; need to add a version check.
                return false;
            }
            bool add_parens = (op == STNODE_OP_AND || op == STNODE_OP_OR) && op != parent_bool_op && parent_bool_op != STNODE_OP_UNINITIALIZED;
            if (add_parens) {
                g_string_append_c(falco_rule, '(');
            }
            if (!visit_dfilter_node(left, op, falco_rule)) {
                return false;
            }
            g_string_append_printf(falco_rule, " %s ", op_str);
            if (!visit_dfilter_node(right, op, falco_rule)) {
                return false;
            }
            if (add_parens) {
                g_string_append_c(falco_rule, ')');
            }
        }
        else if (left) {
            op = op == STNODE_OP_NOT ? op : parent_bool_op;
            if (falco_rule->len > 0) {
                g_string_append_c(falco_rule, ' ');
            }
            g_string_append_printf(falco_rule, "%s ", op_str);
            if (!visit_dfilter_node(left, op, falco_rule)) {
                return false;
            }
        }
        else if (right) {
            ws_assert_not_reached();
        }
    }
    else if (stnode_type_id(node) == STTYPE_SET) {
        return false;
    }
    else if (stnode_type_id(node) == STTYPE_FUNCTION) {
        return false;
    }
    else if (stnode_type_id(node) == STTYPE_FIELD) {
        header_field_info *hfinfo = sttype_field_hfinfo(node);
        char *filtercheck = hfinfo_to_filtercheck(hfinfo);
        if (!filtercheck) {
            return false;
        }
        g_string_append_printf(falco_rule, "%s", filtercheck);
        g_free(filtercheck);
    }
    else if (stnode_type_id(node) == STTYPE_FVALUE) {
        g_string_append_printf(falco_rule, "%s", stnode_tostr(node, true));
    }
    else {
        g_string_append_printf(falco_rule, "%s", stnode_type_name(node));
    }

    return true;
}

bool dfilter_to_falco_rule(stnode_t *root_node, GString *falco_rule) {
    return visit_dfilter_node(root_node, STNODE_OP_UNINITIALIZED, falco_rule);
}

// Stash some useful hf ids.
static int field_hf_id_container_id;
static int field_hf_id_evt_buffer;
static int field_hf_id_evt_is_io_write;
static int field_hf_id_fd_containername;
static int field_hf_id_fd_name;
static int field_hf_id_fd_num;
static int field_hf_id_proc_name;
static int field_hf_id_proc_pid;
static int field_hf_id_thread_tid;

static void
create_source_hfids(bridge_info* bi)
{
    /*
     * Initialize the plugin
     */
    bi->source_id = get_sinsp_source_id(bi->ssi);

    size_t tot_fields = get_sinsp_source_nfields(bi->ssi);
    bi->visible_fields = 0;
    bi->addr_fields = 0;
    sinsp_field_info_t sfi;
    bi->num_conversation_filters = 0;

    for (size_t j = 0; j < tot_fields; j++) {
        get_sinsp_source_field_info(bi->ssi, j, &sfi);
        if (sfi.skip) {
            /*
             * Skip special fields (lists and tables).
             * XXX Should we keep them and call proto_item_set_hidden?
             */
            continue;
        }
        if (sfi.is_numeric_address || is_source_address_field(sfi.type, sfi.abbrev)) {
            bi->addr_fields++;
        }
        bi->visible_fields++;

        if (sfi.is_conversation || is_conversation_field(sfi.type, sfi.abbrev)) {
            bi->num_conversation_filters++;
        }
    }

    if (bi->visible_fields) {
        bi->hf = (hf_register_info*)wmem_alloc(wmem_epan_scope(), bi->visible_fields * sizeof(hf_register_info));
        bi->hf_ids = (int*)wmem_alloc0(wmem_epan_scope(), bi->visible_fields * sizeof(int));
        bi->field_ids = (int*)wmem_alloc(wmem_epan_scope(), bi->visible_fields * sizeof(int));
        bi->field_flags = (uint32_t*)wmem_alloc(wmem_epan_scope(), bi->visible_fields * sizeof(uint32_t));

        if (bi->addr_fields) {
            bi->hf_id_to_addr_id = (int *)wmem_alloc(wmem_epan_scope(), bi->visible_fields * sizeof(int));
            bi->hf_v4 = (hf_register_info*)wmem_alloc(wmem_epan_scope(), bi->addr_fields * sizeof(hf_register_info));
            bi->hf_v4_ids = (int*)wmem_alloc0(wmem_epan_scope(), bi->addr_fields * sizeof(int));
            bi->hf_v6 = (hf_register_info*)wmem_alloc(wmem_epan_scope(), bi->addr_fields * sizeof(hf_register_info));
            bi->hf_v6_ids = (int*)wmem_alloc0(wmem_epan_scope(), bi->addr_fields * sizeof(int));
            bi->hf_geoip = (hf_register_info*)wmem_alloc(wmem_epan_scope(), bi->addr_fields * GEOIP_FIELD_COUNT * sizeof(hf_register_info));
            bi->hf_geoip_ids = (int*)wmem_alloc0(wmem_epan_scope(), bi->addr_fields * GEOIP_FIELD_COUNT * sizeof(int));
        }

        if (bi->num_conversation_filters) {
            bi->conversation_filters = (conv_filter_info *)wmem_alloc(wmem_epan_scope(), bi->num_conversation_filters * sizeof (conv_filter_info));
        }

        uint32_t fld_cnt = 0;
        size_t conv_fld_cnt = 0;
        uint32_t addr_fld_cnt = 0;

        for (size_t j = 0; j < tot_fields; j++)
        {
            get_sinsp_source_field_info(bi->ssi, j, &sfi);

            if (sfi.skip) {
                /*
                 * Skip special fields (lists and tables).
                 */
                continue;
            }

            ws_assert(fld_cnt < bi->visible_fields);
            bi->field_ids[fld_cnt] = (int) j;
            bi->field_flags[fld_cnt] = BFF_NONE;

            enum ftenum ftype = sfi.type;

            // Display formats
            int fdisplay = BASE_NONE;
            switch (sfi.type) {
            case FT_STRINGZ:
            case FT_BOOLEAN:
            case FT_BYTES:
                break;
            case FT_RELATIVE_TIME:
            case FT_ABSOLUTE_TIME:
                fdisplay = BASE_DEC;
                break;
            case FT_INT8:
            case FT_INT16:
            case FT_INT32:
            case FT_INT64:
            case FT_DOUBLE:
                // This differs from libsinsp
                fdisplay = BASE_DEC;
                break;
            case FT_UINT8:
            case FT_UINT16:
            case FT_UINT32:
            case FT_UINT64:
                switch (sfi.display_format) {
                case SFDF_DECIMAL:
                    fdisplay = BASE_DEC;
                    break;
                case SFDF_HEXADECIMAL:
                    fdisplay = BASE_HEX;
                    break;
                case SFDF_OCTAL:
                    fdisplay = BASE_OCT;
                    break;
                default:
                    THROW_FORMATTED(DissectorError, "error in Falco Events plugin %s: format %d for field %s is not supported",
                        get_sinsp_source_name(bi->ssi), sfi.display_format, sfi.abbrev);
                }
                break;
            default:
                ftype = FT_NONE;
                ws_warning("plugin %s: type of field %s (%d) is not supported",
                    get_sinsp_source_name(bi->ssi),
                    sfi.abbrev, sfi.type);
            }

            if(strlen(sfi.display) == 0) {
                // Shouldn't happen since get_sinsp_source_field_info falls back to the filter name.
                THROW_FORMATTED(DissectorError, "error in Falco Events plugin %s: field %s is missing display name",
                   get_sinsp_source_name(bi->ssi), sfi.abbrev);
            }

            hf_register_info finfo = {
                bi->hf_ids + fld_cnt,
                {
                    wmem_strdup(wmem_epan_scope(), sfi.display), wmem_strdup(wmem_epan_scope(), sfi.abbrev),
                    ftype, fdisplay,
                    NULL, 0x0,
                    wmem_strdup(wmem_epan_scope(), sfi.description), HFILL
                }
            };
            bi->hf[fld_cnt] = finfo;

            if (sfi.is_conversation || is_conversation_field(sfi.type, sfi.abbrev)) {
                ws_assert(conv_fld_cnt < bi->num_conversation_filters);
                bi->field_flags[fld_cnt] |= BFF_CONVERSATION;
                bi->conversation_filters[conv_fld_cnt].field_info = &bi->hf[fld_cnt];
                bi->conversation_filters[conv_fld_cnt].strbuf = wmem_strbuf_new(wmem_epan_scope(), "");

                const char *source_name = get_sinsp_source_name(bi->ssi);
                const char *conv_filter_name = wmem_strdup_printf(wmem_epan_scope(), "%s %s", source_name, bi->hf[fld_cnt].hfinfo.name);
                register_log_conversation_filter(source_name, conv_filter_name, is_filter_valid, build_conversation_filter, &bi->conversation_filters[conv_fld_cnt]);
                if (conv_fld_cnt == 0) {
                    add_conversation_filter_protocol(source_name);
                }
                conv_fld_cnt++;
            }

            if (sfi.is_info) {
                bi->field_flags[fld_cnt] |= BFF_INFO;
            }

            if (sfi.is_numeric_address || is_source_address_field(sfi.type, sfi.abbrev)) {
                ws_assert(addr_fld_cnt < bi->addr_fields);
                bi->hf_id_to_addr_id[fld_cnt] = addr_fld_cnt;

                hf_register_info finfo_v4 = {
                    bi->hf_v4_ids + addr_fld_cnt,
                    {
                        wmem_strdup_printf(wmem_epan_scope(), "%s (IPv4)", sfi.display),
                        wmem_strdup_printf(wmem_epan_scope(), "%s.v4", sfi.abbrev),
                        FT_IPv4, BASE_NONE,
                        NULL, 0x0,
                        NULL, HFILL
                    }
                };
                bi->hf_v4[addr_fld_cnt] = finfo_v4;

                hf_register_info finfo_v6 = {
                    bi->hf_v6_ids + addr_fld_cnt,
                    {
                        wmem_strdup_printf(wmem_epan_scope(), "%s (IPv6)", sfi.display),
                        wmem_strdup_printf(wmem_epan_scope(), "%s.v6", sfi.abbrev),
                        FT_IPv6, BASE_NONE,
                        NULL, 0x0,
                        NULL, HFILL
                    }
                };
                bi->hf_v6[addr_fld_cnt] = finfo_v6;

                for (int gidx = 0; gidx < GEOIP_FIELD_COUNT; gidx++) {
                    const char *name = NULL;
                    const char *abbrev = NULL;
                    ftype = FT_STRING;
                    fdisplay = BASE_NONE;
                    switch (gidx) {
                    case GEOIP_SUMMARY:
                        name = wmem_strdup_printf(wmem_epan_scope(), "%s GeoIP Information", sfi.display);
                        abbrev = wmem_strdup_printf(wmem_epan_scope(), "%s.geoip", sfi.abbrev);
                        break;
                    case GEOIP_COUNTRY:
                        name = wmem_strdup_printf(wmem_epan_scope(), "%s GeoIP Country", sfi.display);
                        abbrev = wmem_strdup_printf(wmem_epan_scope(), "%s.geoip.country", sfi.abbrev);
                        break;
                    case GEOIP_COUNTRY_ISO:
                        name = wmem_strdup_printf(wmem_epan_scope(), "%s GeoIP ISO Two Letter Country Code", sfi.display);
                        abbrev = wmem_strdup_printf(wmem_epan_scope(), "%s.geoip.country_iso", sfi.abbrev);
                        break;
                    case GEOIP_CITY:
                        name = wmem_strdup_printf(wmem_epan_scope(), "%s GeoIP City", sfi.display);
                        abbrev = wmem_strdup_printf(wmem_epan_scope(), "%s.geoip.city", sfi.abbrev);
                        break;
                    case GEOIP_AS_NUMBER:
                        name = wmem_strdup_printf(wmem_epan_scope(), "%s GeoIP AS Number", sfi.display);
                        abbrev = wmem_strdup_printf(wmem_epan_scope(), "%s.geoip.asnum", sfi.abbrev);
                        ftype = FT_UINT32;
                        fdisplay = BASE_DEC;
                        break;
                    case GEOIP_AS_ORG:
                        name = wmem_strdup_printf(wmem_epan_scope(), "%s GeoIP AS Organization", sfi.display);
                        abbrev = wmem_strdup_printf(wmem_epan_scope(), "%s.geoip.org", sfi.abbrev);
                        break;
                    case GEOIP_LATITUDE:
                        name = wmem_strdup_printf(wmem_epan_scope(), "%s GeoIP Latitude", sfi.display);
                        abbrev = wmem_strdup_printf(wmem_epan_scope(), "%s.geoip.lat", sfi.abbrev);
                        ftype = FT_DOUBLE;
                        break;
                    case GEOIP_LONGITUDE:
                        name = wmem_strdup_printf(wmem_epan_scope(), "%s GeoIP Longitude", sfi.display);
                        abbrev = wmem_strdup_printf(wmem_epan_scope(), "%s.geoip.lon", sfi.abbrev);
                        ftype = FT_DOUBLE;
                        break;
                    }

                    hf_register_info finfo_geo = {
                        bi->hf_geoip_ids + (addr_fld_cnt * GEOIP_FIELD_COUNT) + gidx,
                        {
                            name,
                            abbrev,
                            ftype, fdisplay,
                            NULL, 0x0,
                            NULL, HFILL
                        }
                    };
                    bi->hf_geoip[(addr_fld_cnt * GEOIP_FIELD_COUNT) + gidx] = finfo_geo;
                }

                addr_fld_cnt++;
            } else if (bi->hf_id_to_addr_id) {
                bi->hf_id_to_addr_id[fld_cnt] = -1;
            }
            fld_cnt++;
        }

        // XXX We should put these under the correct protocol instead of "falcoevents".
        proto_register_field_array(proto_falco_events, bi->hf, fld_cnt);
        if (addr_fld_cnt) {
            proto_register_field_array(proto_falco_events, bi->hf_v4, addr_fld_cnt);
            proto_register_field_array(proto_falco_events, bi->hf_v6, addr_fld_cnt);
            proto_register_field_array(proto_falco_events, bi->hf_geoip, addr_fld_cnt * GEOIP_FIELD_COUNT);
        }

        // Useful hf ids
        for (size_t idx = 0; idx < fld_cnt; idx++) {
            header_field_info *hfinfo = &bi->hf[idx].hfinfo;

            switch(hfinfo->type) {
            case FT_STRINGZ:
                if (strcmp(hfinfo->abbrev, "container.id") == 0) {
                    field_hf_id_container_id = hfinfo->id;
                } else if (strcmp(hfinfo->abbrev, "fd.containername") == 0) {
                    field_hf_id_fd_containername = hfinfo->id;
                } else if (strcmp(hfinfo->abbrev, "fd.name") == 0) {
                    field_hf_id_fd_name = hfinfo->id;
                } else if (strcmp(hfinfo->abbrev, "proc.name") == 0) {
                    field_hf_id_proc_name = hfinfo->id;
                }
                break;
            case FT_INT64:
                if (strcmp(hfinfo->abbrev, "fd.num") == 0) {
                    field_hf_id_fd_num = hfinfo->id;
                } else if (strcmp(hfinfo->abbrev, "proc.pid") == 0) {
                    field_hf_id_proc_pid = hfinfo->id;
                } else if (strcmp(hfinfo->abbrev, "thread.tid") == 0) {
                    field_hf_id_thread_tid = hfinfo->id;
                }
                break;
            case FT_BYTES:
                if (strcmp(hfinfo->abbrev, "evt.buffer") == 0) {
                    field_hf_id_evt_buffer = hfinfo->id;
                }
                break;
            case FT_BOOLEAN:
                if (strcmp(hfinfo->abbrev, "evt.is_io_write") == 0) {
                    field_hf_id_evt_is_io_write = hfinfo->id;
                }
            default:
                break;
            }
        }
    }
}

#define K8SAUDIT_PLUGIN_ID    1
#define CLOUDTRAIL_PLUGIN_ID  2
#define GCPAUDIT_PLUGIN_ID   12

// Plugins whose data should be displayed as JSON.
// XXX This should probably be a preference.
static const uint32_t json_plugins[] = {K8SAUDIT_PLUGIN_ID, CLOUDTRAIL_PLUGIN_ID, GCPAUDIT_PLUGIN_ID};

static const value_string source_id_to_name[] = {
    { K8SAUDIT_PLUGIN_ID,   "Kubernetes Audit Logs" },
    { CLOUDTRAIL_PLUGIN_ID, "AWS CloudTrail" },
    { GCPAUDIT_PLUGIN_ID,   "Google Cloud Audit Logs" },
    { 0, NULL }
};

void
import_plugin(char* fname)
{
    nbridges++;
    bridge_info* bi = &bridges[nbridges - 1];

    char *err_str = create_sinsp_plugin_source(sinsp_span, fname, &(bi->ssi));
    if (err_str) {
        nbridges--;
        report_failure("Unable to load sinsp plugin %s: %s.", fname, err_str);
        g_free(err_str);
        return;
    }

    create_source_hfids(bi);

    const char *source_name = get_sinsp_source_name(bi->ssi);
    const char *plugin_name = try_val_to_str(bi->source_id, source_id_to_name);
    if (!plugin_name) {
        plugin_name = g_strdup_printf("%s Falco Events Plugin", source_name);
    }
    bi->proto = proto_register_protocol(plugin_name, source_name, source_name);

    bi->media_type = DS_MEDIA_TYPE_APPLICATION_OCTET_STREAM;
    for (size_t i = 0; i < array_length(json_plugins); i++) {
        if (json_plugins[i] == bi->source_id) {
            bi->media_type = DS_MEDIA_TYPE_APPLICATION_JSON;
        }
    }

    static dissector_handle_t ct_handle;
    ct_handle = create_dissector_handle(dissect_sinsp_plugin, bi->proto);
    dissector_add_uint("falcoevents.id", bi->source_id, ct_handle);
}

static void
on_app_exit(void)
{
    // XXX This currently crashes in a sinsp thread.
    // destroy_sinsp_span(sinsp_span);
    sinsp_span = NULL;
}

static bool
extract_syscall_conversation_fields (packet_info *pinfo, falco_conv_filter_fields* args) {
    if (!proto_is_protocol_enabled(find_protocol_by_id(proto_falco_events))) {
        // get_extracted_syscall_source_fields will fail noisily, so just bail out here.
        return false;
    }

    args->container_id = NULL;
    args->pid = -1;
    args->tid = -1;
    args->fd = -1;
    args->fd_containername = NULL;

    // Syscalls are always the bridge with source_id 0.
    bridge_info* bi = get_bridge_info(0);

    sinsp_field_extract_t *sinsp_fields = NULL;
    uint32_t sinsp_fields_count = 0;
    void* sinsp_evt_info;
    bool rc = get_extracted_syscall_source_fields(sinsp_span, pinfo->fd->num, &sinsp_fields, &sinsp_fields_count, &sinsp_evt_info);

    if (!rc) {
        REPORT_DISSECTOR_BUG("cannot extract falco conversation fields for event %" PRIu32, pinfo->fd->num);
    }

    for (uint32_t hf_idx = 0, sf_idx = 0; hf_idx < bi->visible_fields && sf_idx < sinsp_fields_count; hf_idx++) {
        if (sinsp_fields[sf_idx].field_idx != hf_idx) {
            continue;
        }

        header_field_info* hfinfo = &(bi->hf[hf_idx].hfinfo);

        if (hfinfo->id == field_hf_id_container_id) {
            args->container_id = get_str_value(sinsp_fields, sf_idx);
            // if (args->container_id == NULL) {
            //     REPORT_DISSECTOR_BUG("cannot extract the container ID for event %" PRIu32, pinfo->fd->num);
            // }
        }

        if (hfinfo->id == field_hf_id_proc_pid) {
            args->pid = sinsp_fields[sf_idx].res.u64;
        }

        if (hfinfo->id == field_hf_id_thread_tid) {
            args->tid = sinsp_fields[sf_idx].res.u64;
        }

        if (hfinfo->id == field_hf_id_fd_num) {
            args->fd = sinsp_fields[sf_idx].res.u64;
        }

        if (hfinfo->id == field_hf_id_fd_containername) {
            args->fd_containername = get_str_value(sinsp_fields, sf_idx);
        }

        sf_idx++;
    }

    // args->fd=-1 means that either there's no FD (e.g. a clone syscall), or that the FD is not a valid one (e.g., failed open).
    if (args->fd == -1) {
        return false;
    }

    return true;
}

static bool sysdig_syscall_filter_valid(packet_info *pinfo, void *user_data _U_) {
    if (!proto_is_frame_protocol(pinfo->layers, "sysdig")) {
        return false;
    }

    // This only supports the syscall source.
    if (pinfo->rec->rec_header.syscall_header.event_type == FALCO_PPME_PLUGINEVENT_E) {
        return false;
    }

    return true;
}

static bool sysdig_syscall_container_filter_valid(packet_info *pinfo, void *user_data) {
    if (!sysdig_syscall_filter_valid(pinfo, user_data)) {
        return false;
    }

    falco_conv_filter_fields cff;
    if (!extract_syscall_conversation_fields(pinfo, &cff)) {
        return false;
    }

    return cff.container_id != NULL;
}

static bool sysdig_syscall_fd_filter_valid(packet_info *pinfo, void *user_data) {
    if (!sysdig_syscall_filter_valid(pinfo, user_data)) {
        return false;
    }

    falco_conv_filter_fields cff;
    return extract_syscall_conversation_fields(pinfo, &cff);
}

static char* sysdig_container_build_filter(packet_info *pinfo, void *user_data _U_) {
    falco_conv_filter_fields cff;
    extract_syscall_conversation_fields(pinfo, &cff);
    if (cff.container_id) {
        return ws_strdup_printf("container.id==\"%s\"", cff.container_id);
    }
    return NULL;
}

static char* sysdig_proc_build_filter(packet_info *pinfo, void *user_data _U_) {
    falco_conv_filter_fields cff;
    extract_syscall_conversation_fields(pinfo, &cff);
    if (cff.container_id) {
        return ws_strdup_printf("container.id==\"%s\" && proc.pid==%" PRId64, cff.container_id, cff.pid);
    } else {
        return ws_strdup_printf("proc.pid==%" PRId64, cff.pid);
    }
}

static char* sysdig_procdescendants_build_filter(packet_info *pinfo, void *user_data _U_) {
    falco_conv_filter_fields cff;
    extract_syscall_conversation_fields(pinfo, &cff);

    if (cff.container_id) {
        return ws_strdup_printf("container.id==\"%s\" && (proc.pid==%" PRId64 " || proc.apid.1==%" PRId64 " || proc.apid.2==%" PRId64 " || proc.apid.3==%" PRId64 " || proc.apid.4==%" PRId64 ")",
            cff.container_id,
            cff.pid,
            cff.pid,
            cff.pid,
            cff.pid,
            cff.pid);
    } else {
        return ws_strdup_printf("proc.pid==%" PRId64 " || proc.apid.1==%" PRId64 " || proc.apid.2==%" PRId64 " || proc.apid.3==%" PRId64 " || proc.apid.4==%" PRId64,
            cff.pid,
            cff.pid,
            cff.pid,
            cff.pid,
            cff.pid);
    }
}

static char* sysdig_thread_build_filter(packet_info *pinfo, void *user_data _U_) {
    falco_conv_filter_fields cff;
    extract_syscall_conversation_fields(pinfo, &cff);
    if (cff.container_id) {
        return ws_strdup_printf("container.id==\"%s\" && thread.tid==%" PRIu64, cff.container_id, cff.tid);
    } else {
        return ws_strdup_printf("thread.tid==%" PRId64, cff.tid);
    }
}

static const fd_stream_info* get_fd_stream_info(packet_info *pinfo) {
    // This only supports the syscall source.
    if (pinfo->rec->rec_header.syscall_header.event_type == FALCO_PPME_PLUGINEVENT_E) {
        return false;
    }

    falco_conv_filter_fields cff;
    extract_syscall_conversation_fields(pinfo, &cff);
    if (!cff.container_id) {
        cff.container_id = DEFAULT_CONTAINER_ID;
    }

    conversation_element_t fd_follow_conv_els[7] = {
        { .type = CE_INT, .int_val = field_hf_id_container_id },
        { .type = CE_STRING, .str_val = cff.container_id },
        { .type = CE_INT, .int_val = field_hf_id_proc_pid },
        { .type = CE_INT64, .int64_val = cff.pid },
        { .type = CE_INT, .int_val = field_hf_id_fd_num },
        { .type = CE_INT64, .int64_val = cff.fd },
        { .type = CE_CONVERSATION_TYPE, .conversation_type_val = CONVERSATION_LOG },
    };

    conversation_t *conv;

    conv = find_conversation_full(pinfo->fd->num, fd_follow_conv_els);
    if (conv) {
        return (fd_stream_info*) conversation_get_proto_data(conv, proto_syscalls[SSC_FD]);
    }
    return NULL;
}

static char* sysdig_fd_build_filter(packet_info *pinfo, void *user_data _U_) {
    const fd_stream_info* fsi = get_fd_stream_info(pinfo);
    if (fsi) {
        return ws_strdup_printf("falcoevents.fd.stream eq %u", fsi->stream_index);
    }
    return NULL;
}

static char *fd_follow_conv_filter(epan_dissect_t *edt _U_, packet_info *pinfo, unsigned *stream, unsigned *sub_stream _U_)
{
    const fd_stream_info* fsi = get_fd_stream_info(pinfo);
    if (fsi) {
        *stream = fsi->stream_index;
        return ws_strdup_printf("falcoevents.fd.stream eq %u", fsi->stream_index);
    }
    return NULL;
}

static char *fd_follow_index_filter(unsigned stream, unsigned sub_stream _U_)
{
    return ws_strdup_printf("falcoevents.fd.stream eq %u", stream);
}

static char *fd_follow_address_filter(address *src_addr _U_, address *dst_addr _U_, int src_port _U_, int dst_port _U_)
{
    return NULL;
}

char *
fd_port_to_display(wmem_allocator_t *allocator _U_, unsigned port _U_)
{
    return NULL;
}

tap_packet_status
fd_tap_listener(void *tapdata, packet_info *pinfo,
                      epan_dissect_t *edt _U_, const void *data, tap_flags_t flags _U_)
{
    follow_record_t *follow_record;
    follow_info_t *follow_info = (follow_info_t *)tapdata;
    fd_follow_tap_info *tap_info = (fd_follow_tap_info *)data;
    bool is_server;

    is_server = tap_info->is_write;

    follow_record = g_new0(follow_record_t, 1);
    follow_record->is_server = is_server;
    follow_record->packet_num = pinfo->fd->num;
    follow_record->abs_ts = pinfo->fd->abs_ts;
    follow_record->data = g_byte_array_append(g_byte_array_new(),
                                              tap_info->data,
                                              tap_info->datalen);

    follow_info->bytes_written[is_server] += follow_record->data->len;
    follow_info->payload = g_list_prepend(follow_info->payload, follow_record);

    return TAP_PACKET_DONT_REDRAW;
}

uint32_t get_fd_stream_count(void)
{
    return fd_stream_count;
}

static bridge_info*
get_bridge_info(uint32_t source_id)
{
    if (source_id == 0) {
        return &bridges[0];
    }

    for(size_t j = 0; j < nbridges; j++)
    {
        if(bridges[j].source_id == source_id)
        {
            return &bridges[j];
        }
    }

    return NULL;
}

static int
dissect_falco_event(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *epd_p)
{
    int encoding = pinfo->rec->rec_header.syscall_header.byte_order == G_BIG_ENDIAN ? ENC_BIG_ENDIAN : ENC_LITTLE_ENDIAN;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "Falco Events");

    // Some syscall events don't have any data. Make sure we don't return 0 in that case
    // so that things like "protocols in frame" work.
    int consumed = tvb_captured_length(tvb);
    if (consumed == 0) {
        consumed = 1;
    }

    // https://github.com/falcosecurity/libs/blob/9c942f27/userspace/libscap/scap.c#L1900

    uint32_t source_id = 0;
    if (pinfo->rec->rec_type == REC_TYPE_SYSCALL && pinfo->rec->rec_header.syscall_header.event_type == FALCO_PPME_PLUGINEVENT_E) {
        source_id = tvb_get_uint32(tvb, 8, encoding);
    }

    bridge_info* bi = get_bridge_info(source_id);

    if (bi && bi->source_id == 0) {
        sysdig_event_param_data *event_param_data = (sysdig_event_param_data *) epd_p;
        dissect_sinsp_enriched(tvb, pinfo, tree, bi, event_param_data);
    } else {
        // proto_item *ti = proto_tree_add_item(tree, proto_falco_events, tvb, 0, 12, ENC_NA);
        // proto_tree *fb_tree = proto_item_add_subtree(ti, ett_falco_events);

        // proto_tree_add_item(fb_tree, hf_sdp_source_id_size, tvb, 0, 4, encoding);
        // proto_tree_add_item(fb_tree, hf_sdp_lengths, tvb, 4, 4, encoding);
        /* Clear out stuff in the info column */
        col_clear(pinfo->cinfo,COL_INFO);
        col_add_fstr(pinfo->cinfo, COL_INFO, "Plugin ID: %u", source_id);

        proto_item *idti = proto_tree_add_item(tree, hf_sdp_source_id, tvb, 8, 4, encoding);
        proto_item_set_hidden(idti);
        if (bi == NULL) {
            proto_item_append_text(idti, " (NOT SUPPORTED)");
            col_append_str(pinfo->cinfo, COL_INFO, " (NOT SUPPORTED)");
            return consumed;
        }

        const char *source_name = get_sinsp_source_name(bi->ssi);
        proto_item_append_text(tree, " (%s)", source_name);
        col_append_fstr(pinfo->cinfo, COL_INFO, " (%s)", source_name);

        tvbuff_t *plugin_tvb = tvb;
        if (bi->media_type == DS_MEDIA_TYPE_APPLICATION_JSON) {
            int plugin_data_len = tvb_captured_length_remaining(tvb, 12);
            plugin_tvb = tvb_new_child_real_data(tvb, tvb_get_ptr(tvb, 12, plugin_data_len), plugin_data_len, tvb_reported_length_remaining(tvb, 12));
            struct data_source *source = add_new_data_source(pinfo, plugin_tvb, source_name);
            set_data_source_media_type(source, DS_MEDIA_TYPE_APPLICATION_JSON);
        }
        dissect_sinsp_plugin(plugin_tvb, pinfo, tree, bi);
    }

    return consumed;
}

// We assume that there is no whitespace between key names and their colons.
// This is *probably* safe, but if not we can just parse the buffer if needed.
static bool has_keys(const uint8_t *data, int data_len, const char *keys[]) {
    for (size_t idx = 0; keys[idx]; idx++) {
        if (!ws_memmem(data, data_len, keys[idx], strlen(keys[idx]))) {
            return false;
        }
    }

    return true;
}

static int
dissect_falco_json_event(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data _U_)
{
    uint32_t source_id = 0;

    // https://kubernetes.io/docs/reference/config-api/apiserver-audit.v1/
    const char *k8saudit_keys[] = { "\"kind\":", "\"apiVersion\":", "\"level\":", "\"auditID\":", NULL };
    // https://docs.aws.amazon.com/awscloudtrail/latest/userguide/cloudtrail-event-reference-record-contents.html
    const char *cloudtrail_keys[] = { "\"eventVersion\":", "\"userIdentity\":", "\"eventSource\":", NULL };
    // https://cloud.google.com/logging/docs/reference/v2/rest/v2/LogEntry
    const char *gcpaudit_keys[] = { "\"insertId\":", "\"logName\":", NULL };

    const uint8_t *tvb_data = tvb_get_ptr(tvb, 0, -1);
    int tvb_len = tvb_captured_length(tvb);

    if (has_keys(tvb_data, tvb_len, k8saudit_keys)) {
        source_id = K8SAUDIT_PLUGIN_ID;
    } else if (has_keys(tvb_data, tvb_len, cloudtrail_keys)) {
        source_id = CLOUDTRAIL_PLUGIN_ID;
    } else if (has_keys(tvb_data, tvb_len, gcpaudit_keys)) {
        source_id = GCPAUDIT_PLUGIN_ID;
    }

    bridge_info* bi = get_bridge_info(source_id);

    if (source_id != 0 && bi) {
        set_data_source_media_type(get_data_source_by_tvb(pinfo, tvb), DS_MEDIA_TYPE_APPLICATION_JSON);
        return dissect_sinsp_plugin(tvb, pinfo, tree, bi);
    }
    return 0;
}

int extract_lineage_number(const char *fld_name) {
    char *last_dot = strrchr(fld_name, '.');
    if (last_dot != NULL) {
        return atoi(last_dot + 1);
    }
    return -1;
}

const char* get_str_value(sinsp_field_extract_t *sinsp_fields, uint32_t sf_idx) {
    const char *res_str;
    if (sinsp_fields[sf_idx].res_len < SFE_SMALL_BUF_SIZE) {
        res_str = sinsp_fields[sf_idx].res.small_str;
    } else {
        if (sinsp_fields[sf_idx].res.str == NULL) {
            ws_debug("Field %u has NULL result string", sf_idx);
            return NULL;
        }
        res_str = sinsp_fields[sf_idx].res.str;
    }

    return res_str;
}

static void add_geoip_info(proto_tree *tree, bridge_info * bi, int base_idx, packet_info *pinfo, tvbuff_t *tvb, int offset, int length, const mmdb_lookup_t *lookup) {
    if (!lookup || !lookup->found) {
        return;
    }

    wmem_strbuf_t *summary = wmem_strbuf_new(pinfo->pool, "");
    if (lookup->city) {
        wmem_strbuf_append(summary, lookup->city);
    }
    if (lookup->country_iso) {
        if (wmem_strbuf_get_len(summary) > 0) wmem_strbuf_append(summary, ", ");
        wmem_strbuf_append(summary, lookup->country_iso);
    } else if (lookup->country) {
        if (wmem_strbuf_get_len(summary) > 0) wmem_strbuf_append(summary, ", ");
        wmem_strbuf_append(summary, lookup->country);
    }
    if (lookup->as_number > 0) {
        if (wmem_strbuf_get_len(summary) > 0) wmem_strbuf_append(summary, ", ");
        wmem_strbuf_append_printf(summary, "ASN %u", lookup->as_number);
    }
    if (lookup->as_org) {
        if (wmem_strbuf_get_len(summary) > 0) wmem_strbuf_append(summary, ", ");
        wmem_strbuf_append(summary, lookup->as_org);
    }

    proto_item *geoip_info_item = proto_tree_add_string(tree, bi->hf_geoip_ids[base_idx + GEOIP_SUMMARY], tvb, offset, length, wmem_strbuf_finalize(summary));
    proto_item_set_generated(geoip_info_item);
    proto_tree *geoip_info_tree = proto_item_add_subtree(geoip_info_item, ett_geoip_info);

    proto_item *item;

    if (lookup->city) {
        item = proto_tree_add_string(geoip_info_tree, bi->hf_geoip_ids[base_idx + GEOIP_CITY], tvb, offset, length, lookup->city);
        proto_item_set_generated(item);
    }

    if (lookup->country) {
        item = proto_tree_add_string(geoip_info_tree, bi->hf_geoip_ids[base_idx + GEOIP_COUNTRY], tvb, offset, length, lookup->country);
        proto_item_set_generated(item);
    }

    if (lookup->country_iso) {
        item = proto_tree_add_string(geoip_info_tree, bi->hf_geoip_ids[base_idx + GEOIP_COUNTRY_ISO], tvb, offset, length, lookup->country_iso);
        proto_item_set_generated(item);
    }

    if (lookup->as_number > 0) {
        item = proto_tree_add_uint(geoip_info_tree, bi->hf_geoip_ids[base_idx + GEOIP_AS_NUMBER], tvb, offset, length, lookup->as_number);
        proto_item_set_generated(item);
    }

    if (lookup->as_org) {
        item = proto_tree_add_string(geoip_info_tree, bi->hf_geoip_ids[base_idx + GEOIP_AS_ORG], tvb, offset, length, lookup->as_org);
        proto_item_set_generated(item);
    }

    if (lookup->latitude >= -90.0 && lookup->latitude <= 90.0) {
        item = proto_tree_add_double(geoip_info_tree, bi->hf_geoip_ids[base_idx + GEOIP_LATITUDE], tvb, offset, length, lookup->latitude);
        proto_item_set_generated(item);
    }

    if (lookup->longitude >= -180.0 && lookup->longitude <= 180.0) {
        item = proto_tree_add_double(geoip_info_tree, bi->hf_geoip_ids[base_idx + GEOIP_LONGITUDE], tvb, offset, length, lookup->longitude);
        proto_item_set_generated(item);
    }
}


// Minimal dissector for adding protocol layers.
static int
dissect_syscall_layer(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, void *psldp)
{
    proto_syscall_layer_data *psld = (proto_syscall_layer_data*) psldp;
    if (!psld) {
        return 0;
    }
    psld->proto_ti = proto_tree_add_item(tree, psld->proto_hfi, tvb, psld->offset, psld->length, BASE_NONE);
    return psld->length;
}

static int
dissect_sinsp_enriched(tvbuff_t* tvb, packet_info* pinfo, proto_tree* tree, void* bi_ptr, sysdig_event_param_data *event_param_data)
{
    bridge_info* bi = (bridge_info *) bi_ptr;

    if (!pinfo->fd->visited) {
        if (pinfo->fd->num == 1) {
            // Open the capture file using libsinsp, which reads the meta events
            // at the beginning of the file. We can't call this via register_init_routine
            // because we don't have the file path at that point.
            open_sinsp_capture(sinsp_span, pinfo->rec->rec_header.syscall_header.pathname);
        }
    }

    sinsp_field_extract_t *sinsp_fields = NULL;
    uint32_t sinsp_fields_count = 0;
    void* sinsp_evt_info;
    bool rc = extract_syscall_source_fields(sinsp_span, bi->ssi, pinfo->fd->num, &sinsp_fields, &sinsp_fields_count, &sinsp_evt_info);

    if (!rc) {
        REPORT_DISSECTOR_BUG("Falco plugin %s extract error: %s", get_sinsp_source_name(bi->ssi), get_sinsp_source_last_error(bi->ssi));
    }

    if (sinsp_fields_count == 0) {
        col_append_str(pinfo->cinfo, COL_INFO, " [Internal event]");
        if (!pref_show_internal) {
            pinfo->fd->passed_dfilter = false;
        }
        return tvb_captured_length(tvb);
    }

    proto_tree *parent_trees[NUM_SINSP_SYSCALL_CATEGORIES] = {0};
    proto_tree *lineage_trees[N_PROC_LINEAGE_ENTRIES] = {0};
    bool is_io_write = false;
    const char* io_buffer = NULL;
    uint32_t io_buffer_len = 0;

    const char *container_id = DEFAULT_CONTAINER_ID;
    int64_t proc_pid = -1;
    const char *proc_name = NULL;
    int64_t fd_num = -1;
    const char *fd_name = NULL;

    // Conversation discoverable through conversation_filter_from_pinfo.
    // Used for related event indicators in the packet list.
    // Fields should match sysdig_proc_build_filter.
    conversation_element_t *pinfo_conv_els = NULL; // thread.tid hfid + thread.tid + container.id hfid + container.id + CONVERSATION_LOG

    for (uint32_t hf_idx = 0, sf_idx = 0; hf_idx < bi->visible_fields && sf_idx < sinsp_fields_count; hf_idx++) {
        if (sinsp_fields[sf_idx].field_idx != hf_idx) {
            continue;
        }

        header_field_info* hfinfo = &(bi->hf[hf_idx].hfinfo);

        proto_tree *ti = NULL;


        // XXX Should we add this back?
//        if (sinsp_fields[sf_idx].type != hfinfo->type) {
//            REPORT_DISSECTOR_BUG("Field %s has an unrecognized or mismatched type %u != %u",
//                                 hfinfo->abbrev, sinsp_fields[sf_idx].type, hfinfo->type);
//        }

        sinsp_syscall_category_e parent_category = get_syscall_parent_category(bi->ssi, sinsp_fields[sf_idx].field_idx);
        if (!parent_trees[parent_category]) {
            int bytes_offset = 0;
            uint32_t bytes_length = 0;
            // Make sure "evt" and "fd" are in pinfo->layers. We should probably do this for each
            // undotted syscall category once issue 12368 is fixed, and each undotted category
            // should probably be a full-blown dissector.
            if (parent_category == SSC_EVENT) {
                bytes_length = tvb_captured_length(tvb);
                proto_syscall_layer_data psld = {proto_syscalls[SSC_EVENT], 0, (int) bytes_length, NULL };
                call_dissector_only(syscall_evt_dissector_handle, tvb, pinfo, tree, &psld);
                ti = psld.proto_ti;
            } else if (parent_category == SSC_FD) {
                bytes_offset = event_param_data->data_bytes_offset;
                bytes_length = event_param_data->data_bytes_length;
                proto_syscall_layer_data psld = {proto_syscalls[SSC_FD], bytes_offset, (int) bytes_length, NULL };
                call_dissector_only(syscall_fd_dissector_handle, tvb, pinfo, tree, &psld);
                ti = psld.proto_ti;
            }
            if (!ti) {
                ti = proto_tree_add_item(tree, proto_syscalls[parent_category], tvb, bytes_offset, bytes_length, BASE_NONE);
            }
            parent_trees[parent_category] = proto_item_add_subtree(ti, ett_syscalls[parent_category]);
        }
        proto_tree *parent_tree = parent_trees[parent_category];

        if (parent_category == SSC_PROCLINEAGE) {
            int32_t lnum = extract_lineage_number(hfinfo->abbrev);
            if (lnum == -1) {
                ws_warning("Invalid lineage field name %s", hfinfo->abbrev);
            }

            if (!lineage_trees[lnum]) {
                const char* res_str = get_str_value(sinsp_fields, sf_idx);
                if (res_str == NULL) {
                    ws_warning("empty value for field %s", hfinfo->abbrev);
                }

                lineage_trees[lnum] = proto_tree_add_subtree_format(parent_tree, tvb, 0, 0, ett_lineage[0], NULL, "%" PRIu32 ". %s", lnum, res_str);
                sf_idx++;
                continue;
            }

            parent_tree = lineage_trees[lnum];
        }

        int32_t arg_num;
#define EVT_ARG_PFX "evt.arg."
        if (! (g_str_has_prefix(hfinfo->abbrev, EVT_ARG_PFX) && ws_strtoi32(hfinfo->abbrev + sizeof(EVT_ARG_PFX) - 1, NULL, &arg_num)) ) {
            arg_num = -1;
        }

        if (hfinfo->id == field_hf_id_evt_is_io_write) {
            is_io_write = sinsp_fields[sf_idx].res.boolean;
        }
        if (hfinfo->id == field_hf_id_evt_buffer) {
            io_buffer = sinsp_fields[sf_idx].res.str;
            io_buffer_len = sinsp_fields[sf_idx].res_len;
        }

        switch (hfinfo->type) {
        case FT_INT8:
        case FT_INT16:
        case FT_INT32:
            proto_tree_add_int(parent_tree, bi->hf_ids[hf_idx], tvb, 0, 0, sinsp_fields[sf_idx].res.i32);
            break;
        case FT_INT64:
            proto_tree_add_int64(parent_tree, bi->hf_ids[hf_idx], tvb, 0, 0, sinsp_fields[sf_idx].res.i64);
            if (hfinfo->id == field_hf_id_thread_tid) {
                if (!pinfo_conv_els) {
                    pinfo_conv_els = wmem_alloc0(pinfo->pool, sizeof(conversation_element_t) * 5);
                    pinfo_conv_els[0].type = CE_INT;
                    pinfo_conv_els[1].type = CE_INT64;
                    pinfo_conv_els[2].type = CE_INT;
                    pinfo_conv_els[3].type = CE_STRING;
                }
                pinfo_conv_els[0].int_val = hfinfo->id;
                pinfo_conv_els[1].int64_val = sinsp_fields[sf_idx].res.i64;
            } else if (hfinfo->id == field_hf_id_proc_pid) {
                proc_pid = sinsp_fields[sf_idx].res.i64;
            } else if (hfinfo->id == field_hf_id_fd_num) {
                fd_num = sinsp_fields[sf_idx].res.i64;
            }
            break;
        case FT_UINT8:
        case FT_UINT16:
        case FT_UINT32:
            proto_tree_add_uint(parent_tree, bi->hf_ids[hf_idx], tvb, 0, 0, sinsp_fields[sf_idx].res.u32);
            break;
        case FT_UINT64:
        case FT_RELATIVE_TIME:
        case FT_ABSOLUTE_TIME:
            proto_tree_add_uint64(parent_tree, bi->hf_ids[hf_idx], tvb, 0, 0, sinsp_fields[sf_idx].res.u64);
            break;
        case FT_STRINGZ:
        {
            const char* res_str = get_str_value(sinsp_fields, sf_idx);
            if (res_str == NULL) {
                continue;
            }

            if (arg_num != -1) {
                // When the field is an argument, we want to display things in a way that includes the argument name and value.
                char* argname = get_evt_arg_name(sinsp_evt_info, arg_num);
                ti = proto_tree_add_string_format(parent_tree, bi->hf_ids[hf_idx], tvb, 0, 0, res_str, "%s: %s", argname, res_str);
            } else {
                ti = proto_tree_add_string(parent_tree, bi->hf_ids[hf_idx], tvb, 0, 0, res_str);
            }

            if (bi->field_flags[hf_idx] & BFF_INFO) {
                col_append_sep_fstr(pinfo->cinfo, COL_INFO, ", ", "%s", res_str);
                // Mark it hidden, otherwise we end up with a bunch of empty "Info" tree items.
                proto_item_set_hidden(ti);
            }

            if (hfinfo->id == field_hf_id_proc_name) {
                proc_name = res_str;
            } else if (hfinfo->id == field_hf_id_fd_name) {
                fd_name = res_str;
            } else if (hfinfo->id == field_hf_id_container_id) {
                container_id = res_str;
                if (pinfo_conv_els) {
                    pinfo_conv_els[2].int_val = hfinfo->id;
                }
            }
        }
            break;
            case FT_BOOLEAN:
                proto_tree_add_boolean(parent_tree, bi->hf_ids[hf_idx], tvb, 0, 0, sinsp_fields[sf_idx].res.boolean);
                break;
            case FT_DOUBLE:
                proto_tree_add_double(parent_tree, bi->hf_ids[hf_idx], tvb, 0, 0, sinsp_fields[sf_idx].res.dbl);
                break;
            case FT_BYTES:
            {
                int addr_fld_idx = bi->hf_id_to_addr_id[hf_idx];
                if (addr_fld_idx < 0) {
                    int bytes_offset = 0;
                    uint32_t bytes_length = 0;
                    if (io_buffer) { // evt.buffer
                        bytes_offset = event_param_data->data_bytes_offset;
                        bytes_length = event_param_data->data_bytes_length;
                    }
                    proto_tree_add_bytes_with_length(parent_tree, bi->hf_ids[hf_idx], tvb, bytes_offset, bytes_length, sinsp_fields[sf_idx].res.str, sinsp_fields[sf_idx].res_len);
                } else {
                    // XXX Need to differentiate between src and dest. Falco libs supply client vs server and local vs remote.
                    const mmdb_lookup_t *lookup = NULL;

                    if (sinsp_fields[sf_idx].res_len == 4) {
                        ws_in4_addr v4_addr;
                        memcpy(&v4_addr, sinsp_fields[sf_idx].res.bytes, 4);
                        proto_tree_add_ipv4(parent_tree, bi->hf_v4_ids[addr_fld_idx], tvb, 0, 0, v4_addr);
                        set_address(&pinfo->net_src, AT_IPv4, sizeof(ws_in4_addr), &v4_addr);
                        copy_address_shallow(&pinfo->src, &pinfo->net_src);
                        lookup = maxmind_db_lookup_ipv4(&v4_addr);
                    } else if (sinsp_fields[sf_idx].res_len == 16) {
                        ws_in6_addr v6_addr;
                        memcpy(&v6_addr, sinsp_fields[sf_idx].res.bytes, 16);
                        proto_tree_add_ipv6(parent_tree, bi->hf_v6_ids[addr_fld_idx], tvb, 0, 0, &v6_addr);
                        set_address(&pinfo->net_src, AT_IPv6, sizeof(ws_in6_addr), &v6_addr);
                        copy_address_shallow(&pinfo->src, &pinfo->net_src);
                        lookup = maxmind_db_lookup_ipv6(&v6_addr);
                    } else {
                        ws_warning("Invalid length %u for address field %u", sinsp_fields[sf_idx].res_len, sf_idx);
                    }

                    if (lookup) {
                        add_geoip_info(tree, bi, bi->hf_geoip_ids[addr_fld_idx * GEOIP_FIELD_COUNT], pinfo, tvb, 0, 0, lookup);
                    }
                    // XXX Add conversation support.
                }
                break;
            }
            default:
                break;
        }
        sf_idx++;
    }

    // Add an fd stream conversation. Used by fd_follow_conv_filter.
    fd_stream_info *fsi = NULL;
    if (proc_pid > 0 && fd_num >= 0) {
        conversation_t *conv = NULL;
        conversation_element_t fd_follow_conv_els[7] = {
            { .type = CE_INT, .int_val = field_hf_id_container_id },
            { .type = CE_STRING, .str_val = container_id },
            { .type = CE_INT, .int_val = field_hf_id_proc_pid },
            { .type = CE_INT64, .int64_val = proc_pid },
            { .type = CE_INT, .int_val = field_hf_id_fd_num },
            { .type = CE_INT64, .int64_val = fd_num },
            { .type = CE_CONVERSATION_TYPE, .conversation_type_val = CONVERSATION_LOG },
        };

        if (!pinfo->fd->visited) {
            bool new_stream = false;
            if (evt_creates_fd(sinsp_evt_info)) {
                // Our process created a descriptor
                conv = conversation_new_full(pinfo->fd->num, fd_follow_conv_els);
                new_stream = true;
            } else {
                // We inherited a descriptor
                conv = find_conversation_full(pinfo->fd->num, fd_follow_conv_els);
                if (!conv) {
                    conv = conversation_new_full(pinfo->fd->num, fd_follow_conv_els);
                    new_stream = true;
                }
            }
            if (new_stream) {
                fsi = wmem_new(wmem_file_scope(), fd_stream_info);
                fsi->stream_index = fd_stream_count;
                conversation_add_proto_data(conv, proto_syscalls[SSC_FD], fsi);
                fd_stream_count++;
            }
        } else {
            conv = find_conversation_full(pinfo->fd->num, fd_follow_conv_els);
        }
        if (conv && !fsi) {
            fsi = (fd_stream_info*) conversation_get_proto_data(conv, proto_syscalls[SSC_FD]);
        }
    }

    if (fsi) {
        proto_tree_add_uint(parent_trees[SSC_FD], hf_fd_stream, tvb, 0, 0, fsi->stream_index);
    }

    if (pinfo_conv_els) {
        pinfo_conv_els[3].str_val = container_id;
        pinfo_conv_els[4].type = CE_CONVERSATION_TYPE;
        pinfo_conv_els[4].conversation_type_val = CONVERSATION_LOG;
        pinfo->conv_elements = pinfo_conv_els;
        find_or_create_conversation(pinfo);
    }

    if (io_buffer_len > 0) {
        if (have_tap_listener(fd_follow_tap)) {
            fd_follow_tap_info *tap_info = wmem_new(pinfo->pool, fd_follow_tap_info);
            tap_info->data = io_buffer;
            tap_info->datalen = io_buffer_len;
            tap_info->is_write = is_io_write;
            tap_queue_packet(fd_follow_tap, pinfo, tap_info);
        }
        if (have_tap_listener(container_io_tap) && proc_name && fd_name) {
            container_io_tap_info *tap_info = wmem_new(pinfo->pool, container_io_tap_info);
            tap_info->proc_name = proc_name;
            tap_info->fd_name = fd_name;
            tap_info->container_id = container_id;
            tap_info->io_bytes = io_buffer_len;
            tap_info->is_write = is_io_write;
            tap_queue_packet(container_io_tap, pinfo, tap_info);
        }
    }

    return tvb_captured_length(tvb);
}

static int
dissect_sinsp_plugin(tvbuff_t* tvb, packet_info* pinfo, proto_tree* tree, void* bi_ptr)
{
    bridge_info* bi = (bridge_info *) bi_ptr;
    unsigned payload_len = tvb_captured_length(tvb);

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "oops");
    /* Clear out stuff in the info column */
    col_clear(pinfo->cinfo, COL_INFO);

    proto_item* ti = proto_tree_add_item(tree, bi->proto, tvb, 0, payload_len, ENC_NA);
    proto_tree* fb_tree = proto_item_add_subtree(ti, ett_sinsp_span);
    // proto_tree *fb_tree = tree;

    uint8_t* payload = (uint8_t*)tvb_get_ptr(tvb, 0, payload_len);

    plugin_field_extract_t *sinsp_fields = (plugin_field_extract_t*) wmem_alloc(pinfo->pool, sizeof(plugin_field_extract_t) * bi->visible_fields);
    for (uint32_t fld_idx = 0; fld_idx < bi->visible_fields; fld_idx++) {
        header_field_info* hfinfo = &(bi->hf[fld_idx].hfinfo);
        plugin_field_extract_t *sfe = &sinsp_fields[fld_idx];

        sfe->field_id = bi->field_ids[fld_idx];
        sfe->field_name = hfinfo->abbrev;
        sfe->type = hfinfo->type == FT_STRINGZ ? FT_STRINGZ : FT_UINT64;
    }

    // If we have a failure, try to dissect what we can first, then bail out with an error.
    bool rc = extract_plugin_source_fields(bi->ssi, pinfo->num, payload, payload_len, pinfo->pool, sinsp_fields, bi->visible_fields);

    if (!rc) {
        REPORT_DISSECTOR_BUG("Falco plugin %s extract error: %s", get_sinsp_source_name(bi->ssi), get_sinsp_source_last_error(bi->ssi));
    }

    for (uint32_t idx = 0; idx < bi->num_conversation_filters; idx++) {
        bi->conversation_filters[idx].is_present = false;
        wmem_strbuf_truncate(bi->conversation_filters[idx].strbuf, 0);
    }

    conversation_element_t *first_conv_els = NULL; // hfid + field val + CONVERSATION_LOG

    for (uint32_t fld_idx = 0; fld_idx < bi->visible_fields; fld_idx++) {
        plugin_field_extract_t *sfe = &sinsp_fields[fld_idx];
        header_field_info* hfinfo = &(bi->hf[fld_idx].hfinfo);

        if (!sfe->is_present) {
            continue;
        }

        conv_filter_info *cur_conv_filter = NULL;
        conversation_element_t *cur_conv_els = NULL;
        if ((bi->field_flags[fld_idx] & BFF_CONVERSATION) != 0) {
            for (uint32_t cf_idx = 0; cf_idx < bi->num_conversation_filters; cf_idx++) {
                if (&(bi->conversation_filters[cf_idx].field_info)->hfinfo == hfinfo) {
                    cur_conv_filter = &bi->conversation_filters[cf_idx];
                    if (!first_conv_els) {
                        first_conv_els = wmem_alloc0(pinfo->pool, sizeof(conversation_element_t) * 3);
                        first_conv_els[0].type = CE_INT;
                        first_conv_els[0].int_val = hfinfo->id;
                        cur_conv_els = first_conv_els;
                    }
                    break;
                }
            }
        }

        proto_item *sf_ti = NULL;
        if (sfe->type == FT_STRINGZ && hfinfo->type == FT_STRINGZ) {
            sf_ti = proto_tree_add_string(fb_tree, bi->hf_ids[fld_idx], tvb, sfe->data_start, sfe->data_length, sfe->res.str);
            if (bi->field_flags[fld_idx] & BFF_INFO) {
                col_append_sep_fstr(pinfo->cinfo, COL_INFO, ", ", "%s", sfe->res.str);
                // Mark it hidden, otherwise we end up with a bunch of empty "Info" tree items.
                proto_item_set_hidden(sf_ti);
            }

            if ((strcmp(hfinfo->abbrev, "ct.response") == 0 ||
                    strcmp(hfinfo->abbrev, "ct.request") == 0 ||
                    strcmp(hfinfo->abbrev, "ct.additionaleventdata") == 0 ||
                    strcmp(hfinfo->abbrev, "ct.resources") == 0 ) &&
                    strcmp(sfe->res.str, "null") != 0) {
                tvbuff_t *json_tvb = tvb_new_subset_length(tvb, sfe->data_start, sfe->data_length);
                proto_tree *json_tree = proto_item_add_subtree(sf_ti, ett_json);
                char *col_info_text = wmem_strdup(pinfo->pool, col_get_text(pinfo->cinfo, COL_INFO));
                call_dissector(json_handle, json_tvb, pinfo, json_tree);

                /* Restore Protocol and Info columns */
                col_set_str(pinfo->cinfo, COL_INFO, col_info_text);
            }
            int addr_fld_idx = bi->hf_id_to_addr_id ? bi->hf_id_to_addr_id[fld_idx] : -1;
            if (addr_fld_idx >= 0) {
                ws_in4_addr v4_addr;
                ws_in6_addr v6_addr;
                proto_tree *addr_tree;
                proto_item *addr_item = NULL;
                const mmdb_lookup_t *lookup = NULL;
                if (ws_inet_pton4(sfe->res.str, &v4_addr)) {
                    addr_tree = proto_item_add_subtree(sf_ti, ett_address);
                    addr_item = proto_tree_add_ipv4(addr_tree, bi->hf_v4_ids[addr_fld_idx], tvb, sfe->data_start, sfe->data_length, v4_addr);
                    set_address(&pinfo->net_src, AT_IPv4, sizeof(ws_in4_addr), &v4_addr);
                    copy_address_shallow(&pinfo->src, &pinfo->net_src);
                    lookup = maxmind_db_lookup_ipv4(&v4_addr);
                } else if (ws_inet_pton6(sfe->res.str, &v6_addr)) {
                    addr_tree = proto_item_add_subtree(sf_ti, ett_address);
                    addr_item = proto_tree_add_ipv6(addr_tree, bi->hf_v6_ids[addr_fld_idx], tvb, sfe->data_start, sfe->data_length, &v6_addr);
                    set_address(&pinfo->net_src, AT_IPv6, sizeof(ws_in6_addr), &v6_addr);
                    copy_address_shallow(&pinfo->src, &pinfo->net_src);
                    lookup = maxmind_db_lookup_ipv6(&v6_addr);
                }
                if (lookup) {
                    add_geoip_info(fb_tree, bi, addr_fld_idx * GEOIP_FIELD_COUNT, pinfo, tvb, sfe->data_start, sfe->data_length, lookup);
                }
                if (addr_item) {
                    proto_item_set_generated(addr_item);
                }
                if (cur_conv_filter) {
                    wmem_strbuf_append(cur_conv_filter->strbuf, sfe->res.str);
                    cur_conv_filter->is_present = true;
                }
                if (cur_conv_els) {
                    cur_conv_els[1].type = CE_ADDRESS;
                    copy_address(&cur_conv_els[1].addr_val, &pinfo->net_src);
                }
            } else {
                if (cur_conv_filter) {
                    wmem_strbuf_append_printf(cur_conv_filter->strbuf, "\"%s\"", sfe->res.str);
                    cur_conv_filter->is_present = true;
                }
                if (cur_conv_els) {
                    cur_conv_els[1].type = CE_STRING;
                    cur_conv_els[1].str_val = wmem_strdup(pinfo->pool, sfe->res.str);
                }
            }
        }
        else if (sfe->type == FT_UINT64 && hfinfo->type == FT_UINT64) {
            proto_tree_add_uint64(fb_tree, bi->hf_ids[fld_idx], tvb, sfe->data_start, sfe->data_length, sfe->res.u64);
            if (cur_conv_filter) {
                switch (hfinfo->display) {
                case BASE_HEX:
                    wmem_strbuf_append_printf(cur_conv_filter->strbuf, "%" PRIx64, sfe->res.u64);
                    break;
                case BASE_OCT:
                    wmem_strbuf_append_printf(cur_conv_filter->strbuf, "%" PRIo64, sfe->res.u64);
                    break;
                default:
                    wmem_strbuf_append_printf(cur_conv_filter->strbuf, "%" PRId64, sfe->res.u64);
                }
                cur_conv_filter->is_present = true;
            }

            if (cur_conv_els) {
                cur_conv_els[1].type = CE_UINT64;
                cur_conv_els[1].uint64_val = sfe->res.u64;
            }
        }
        else {
            REPORT_DISSECTOR_BUG("Field %s has an unrecognized or mismatched type %u != %u",
                hfinfo->abbrev, sfe->type, hfinfo->type);
        }
        if (sf_ti && sfe->is_generated) {
            proto_item_set_generated(sf_ti);
        }
    }

    if (first_conv_els) {
        first_conv_els[2].type = CE_CONVERSATION_TYPE;
        first_conv_els[2].conversation_type_val = CONVERSATION_LOG;
        pinfo->conv_elements = first_conv_els;
//        conversation_t *conv = find_or_create_conversation(pinfo);
//        if (!conv) {
//            conversation_new_full(pinfo->fd->num, pinfo->conv_elements);
//        }
    }

    return payload_len;
}

const char *st_str_container_total_io = "Total";

static void container_io_stats_tree_init(stats_tree* st _U_)
{
    stats_tree_create_node(st, st_str_container_total_io, 0, STAT_DT_INT, true);
    stat_node_set_flags(st, st_str_container_total_io, 0, false, ST_FLG_SORT_TOP);

}

static tap_packet_status container_io_stats_tree_event(stats_tree* st, packet_info* pinfo _U_, epan_dissect_t* edt _U_, const void* tap_info_p, tap_flags_t flags _U_)
{
    const container_io_tap_info* tap_info = (const container_io_tap_info*) tap_info_p;

    increase_stat_node(st, st_str_container_total_io, 0, false, tap_info->io_bytes);
    int container_id_node = increase_stat_node(st, tap_info->container_id, 0, true, tap_info->io_bytes);
    int proc_name_node = increase_stat_node(st, tap_info->proc_name, container_id_node, true, tap_info->io_bytes);
    int fd_name_node = increase_stat_node(st, tap_info->fd_name, proc_name_node, true, tap_info->io_bytes);
    if (tap_info->is_write) {
        increase_stat_node(st, "write", fd_name_node, true, tap_info->io_bytes);
    } else {
        increase_stat_node(st, "read", fd_name_node, true, tap_info->io_bytes);
    }

    return TAP_PACKET_REDRAW;
}

void
proto_reg_handoff_falcoplugin(void)
{
    // Register statistics trees
    stats_tree_cfg *st_config = stats_tree_register_plugin("container_io", "container_io", "Container I/O", 0, container_io_stats_tree_event, container_io_stats_tree_init, NULL);
    stats_tree_set_group(st_config, REGISTER_LOG_STAT_GROUP_UNSORTED);
    stats_tree_set_first_column_name(st_config, "Container, process, and FD I/O");

    json_handle = find_dissector("json");
}

void
proto_register_falcoplugin(void)
{
    // Opening requires a file path, so we do that in dissect_sinsp_enriched.
    register_cleanup_routine(&falco_events_cleanup);

    proto_falco_events = proto_register_protocol("Falco Events", "Falco Events", "falcoevents");
    register_dissector("falcoevents", dissect_falco_event, proto_falco_events);
    proto_register_alias(proto_falco_events, "falcobridge");

    proto_falco_json_events = proto_register_protocol("Falco JSON Events", "Falco JSON Events", "falcojson");
    register_dissector("falcojson", dissect_falco_json_event, proto_falco_json_events);

    // Register the syscall conversation filters.
    // These show up in the "Conversation Filter" and "Colorize Conversation" context menus.
    // The first match is also used for "Go" menu navigation.
    register_log_conversation_filter("falcoevents", "Thread", sysdig_syscall_filter_valid, sysdig_thread_build_filter, NULL);
    register_log_conversation_filter("falcoevents", "Process", sysdig_syscall_filter_valid, sysdig_proc_build_filter, NULL);
    register_log_conversation_filter("falcoevents", "Container", sysdig_syscall_container_filter_valid, sysdig_container_build_filter, NULL);
    register_log_conversation_filter("falcoevents", "Process and Descendants", sysdig_syscall_filter_valid, sysdig_procdescendants_build_filter, NULL);
    register_log_conversation_filter("falcoevents", "File Descriptor", sysdig_syscall_fd_filter_valid, sysdig_fd_build_filter, NULL);
    add_conversation_filter_protocol("falcoevents");

    // Register statistics taps
    container_io_tap = register_tap("container_io");

    // Try to have a 1:1 mapping for as many Sysdig / Falco fields as possible.
    // The exceptions are SSC_EVTARGS and SSC_PROCLINEAGE, which exposes the event arguments in a way that is convenient for the user.
    proto_syscalls[SSC_EVENT] = proto_register_protocol("Event Information", "Falco Event", "evt");
    proto_syscalls[SSC_EVTARGS] = proto_register_protocol("Event Arguments", "Falco Event Info", "evt.arg");
    proto_syscalls[SSC_PROCESS] = proto_register_protocol("Process Information", "Falco Process", "process");
    proto_syscalls[SSC_PROCLINEAGE] = proto_register_protocol("Process Ancestors", "Falco Process Lineage", "proc.aname");
    proto_syscalls[SSC_USER] = proto_register_protocol("User Information", "Falco User", "user");
    proto_syscalls[SSC_GROUP] = proto_register_protocol("Group Information", "Falco Group", "group");
    proto_syscalls[SSC_CONTAINER] = proto_register_protocol("Container Information", "Falco Container", "container");
    proto_syscalls[SSC_FD] = proto_register_protocol("File Descriptor Information", "Falco FD", "fd");
    proto_syscalls[SSC_FS] = proto_register_protocol("Filesystem Information", "Falco FS", "fs");
    // syslog.facility collides with the Syslog dissector, so let syslog fall through to "falco".
    proto_syscalls[SSC_FDLIST] = proto_register_protocol("File Descriptor List", "Falco FD List", "fdlist");
    proto_syscalls[SSC_OTHER] = proto_register_protocol("Unknown or Miscellaneous Falco", "Falco Misc", "falco");

    syscall_evt_dissector_handle = register_dissector("evt", dissect_syscall_layer, proto_syscalls[SSC_EVENT]);
    syscall_fd_dissector_handle = register_dissector("fd", dissect_syscall_layer, proto_syscalls[SSC_FD]);

    // Register the file descriptor "follow" handlers
    fd_follow_tap = register_tap("fd_follow");

    register_follow_stream(proto_syscalls[SSC_FD], "fd_follow", fd_follow_conv_filter, fd_follow_index_filter, fd_follow_address_filter,
                           fd_port_to_display, fd_tap_listener, get_fd_stream_count, NULL);

    // Preferences
    module_t *falco_events_module = prefs_register_protocol(proto_falco_events, NULL);
    prefs_register_bool_preference(falco_events_module, "show_internal_events",
                                   "Show internal events",
                                   "Show internal libsinsp events in the event list.",
                                   &pref_show_internal);


    /*
     * Create the dissector table that we will use to route the dissection to
     * the appropriate Falco plugin.
     */
    ptype_dissector_table = register_dissector_table("falcoevents.id",
                                                     "Falco Events Plugin ID", proto_falco_events, FT_UINT32, BASE_DEC);

    /*
     * Load the plugins
     */
    WS_DIR *dir;
    WS_DIRENT *file;
    char *filename;
    // XXX Falco plugins should probably be installed in a path that reflects
    // the Falco version or its plugin API version.
    char *spdname = g_build_filename(get_plugins_dir(), "falco", NULL);
    char *ppdname = g_build_filename(get_plugins_pers_dir(), "falco", NULL);

    /*
     * We scan the plugins directory twice. The first time we count how many
     * plugins we have, which we need to know in order to allocate the right
     * amount of memory. The second time we actually load and configure
     * each plugin.
     */
    if ((dir = ws_dir_open(spdname, 0, NULL)) != NULL) {
        while ((ws_dir_read_name(dir)) != NULL) {
            nbridges++;
        }
        ws_dir_close(dir);
    }

    if (!files_identical(ppdname, spdname) && (dir = ws_dir_open(ppdname, 0, NULL)) != NULL) {
        while ((ws_dir_read_name(dir)) != NULL) {
            nbridges++;
        }
        ws_dir_close(dir);
    }

    sinsp_span = create_sinsp_span();

    bridges = g_new0(bridge_info, nbridges + 1);

    create_sinsp_syscall_source(sinsp_span, &bridges[0].ssi);

    create_source_hfids(&bridges[0]);
    nbridges = 1;

    if ((dir = ws_dir_open(spdname, 0, NULL)) != NULL) {
        while ((file = ws_dir_read_name(dir)) != NULL) {
            filename = g_build_filename(spdname, ws_dir_get_name(file), NULL);
            import_plugin(filename);
            g_free(filename);
        }
        ws_dir_close(dir);
    }

    if (!files_identical(ppdname, spdname) && (dir = ws_dir_open(ppdname, 0, NULL)) != NULL) {
        while ((file = ws_dir_read_name(dir)) != NULL) {
            filename = g_build_filename(ppdname, ws_dir_get_name(file), NULL);
            import_plugin(filename);
            g_free(filename);
        }
        ws_dir_close(dir);
    }

    g_free(spdname);
    g_free(ppdname);

    /*
     * Setup protocol subtree array
     */
    static int *ett[] = {
        &ett_falco_events,
        &ett_syscalls[SSC_EVENT],
        &ett_syscalls[SSC_EVTARGS],
        &ett_syscalls[SSC_PROCESS],
        &ett_syscalls[SSC_PROCLINEAGE],
        &ett_syscalls[SSC_USER],
        &ett_syscalls[SSC_GROUP],
        &ett_syscalls[SSC_FD],
        &ett_syscalls[SSC_FS],
        &ett_syscalls[SSC_FDLIST],
        &ett_syscalls[SSC_OTHER],
        &ett_sinsp_enriched,
        &ett_sinsp_span,
        &ett_address,
        &ett_json,
        &ett_geoip_info,
    };

    /*
     * Setup process lineage subtree array
     */
    static int *ett_lin[] = {
        &ett_lineage[0],
        &ett_lineage[1],
        &ett_lineage[2],
        &ett_lineage[3],
        &ett_lineage[4],
        &ett_lineage[5],
        &ett_lineage[6],
        &ett_lineage[7],
        &ett_lineage[8],
        &ett_lineage[9],
        &ett_lineage[10],
        &ett_lineage[11],
        &ett_lineage[12],
        &ett_lineage[13],
        &ett_lineage[14],
        &ett_lineage[15],
    };

    proto_register_field_array(proto_falco_events, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));
    proto_register_subtree_array(ett_lin, array_length(ett_lin));

    register_dfilter_translator("Falco rule", dfilter_to_falco_rule);

    register_shutdown_routine(on_app_exit);
}
