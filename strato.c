/* strato.c
 *
 * Text-mode variant of Stratoshark, along the lines of sysdig.
 * Adapted from tshark.c.
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>

#define WS_LOG_DOMAIN LOG_DOMAIN_MAIN

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <limits.h>

#include <wsutil/ws_getopt.h>

#include <errno.h>

#ifdef _WIN32
# include <winsock2.h>
#endif

#ifndef _WIN32
#include <signal.h>
#endif

#include <glib.h>

#include <epan/exceptions.h>
#include <epan/epan.h>

#include <ws_exit_codes.h>
#include <wsutil/clopts_common.h>
#include <wsutil/cmdarg_err.h>
#include <ui/urls.h>
#include <wsutil/filesystem.h>
#include <wsutil/file_util.h>
#include <wsutil/time_util.h>
#include <wsutil/socket.h>
#include <wsutil/privileges.h>
#include <wsutil/please_report_bug.h>
#include <wsutil/wslog.h>
#include <wsutil/ws_assert.h>
#include <wsutil/strtoi.h>
#include <cli_main.h>
#include <wsutil/version_info.h>
#include <wiretap/wtap_opttypes.h>

#include "globals.h"
#include <epan/timestamp.h>
#include <epan/packet.h>
#ifdef HAVE_LUA
#include <epan/wslua/init_wslua.h>
#endif
#include <epan/disabled_protos.h>
#include <epan/prefs.h>
#include <epan/column.h>
#include <epan/decode_as.h>
#include <epan/print.h>
#include <epan/addr_resolv.h>
#include <epan/enterprises.h>
#include <epan/manuf.h>
#include <epan/services.h>
#include <epan/secrets.h>
#include "ui/taps.h"
#include "ui/util.h"
#include "ui/ws_ui_util.h"
#include "ui/decode_as_utils.h"
#include "wsutil/filter_files.h"
#include "ui/cli/strato-tap.h"
#include "ui/cli/tap-exportobject.h"
#include "ui/tap_export_pdu.h"
#include "ui/dissect_opts.h"
#include "ui/failure_message.h"
#include "ui/capture_opts.h"
#if defined(HAVE_LIBSMI)
#include "epan/oids.h"
#endif
#include "epan/maxmind_db.h"
#include <epan/epan_dissect.h>
#include <epan/tap.h>
#include <epan/stat_tap_ui.h>
#include <epan/conversation_table.h>
#include <epan/srt_table.h>
#include <epan/rtd_table.h>
#include <epan/ex-opt.h>
#include <epan/exported_pdu.h>
#include <epan/secrets.h>

#include "capture/capture-pcap-util.h"

#include <epan/funnel.h>

#include <wsutil/application_flavor.h>
#include <wsutil/str_util.h>
#include <wsutil/utf8_entities.h>
#include <wsutil/json_dumper.h>
#include <wsutil/wslog.h>
#include <wsutil/report_message.h>
#ifdef _WIN32
#include <wsutil/win32-utils.h>
#endif

#include "extcap.h"

#ifdef HAVE_PLUGINS
#include <wsutil/codecs.h>
#include <wsutil/plugins.h>
#endif

/* Additional exit codes */
#define INVALID_EXPORT          2
#define INVALID_TAP             2
#define INVALID_CAPTURE         2

#define LONGOPT_EXPORT_OBJECTS          LONGOPT_BASE_APPLICATION+1
#define LONGOPT_COLOR                   LONGOPT_BASE_APPLICATION+2
#define LONGOPT_NO_DUPLICATE_KEYS       LONGOPT_BASE_APPLICATION+3
#define LONGOPT_ELASTIC_MAPPING_FILTER  LONGOPT_BASE_APPLICATION+4
#define LONGOPT_EXPORT_TLS_SESSION_KEYS LONGOPT_BASE_APPLICATION+5
#define LONGOPT_CAPTURE_COMMENT         LONGOPT_BASE_APPLICATION+6
#define LONGOPT_HEXDUMP                 LONGOPT_BASE_APPLICATION+7
#define LONGOPT_SELECTED_FRAME          LONGOPT_BASE_APPLICATION+8
#define LONGOPT_PRINT_TIMERS            LONGOPT_BASE_APPLICATION+9
#define LONGOPT_GLOBAL_PROFILE          LONGOPT_BASE_APPLICATION+10
#define LONGOPT_COMPRESS                LONGOPT_BASE_APPLICATION+11

capture_file cfile;

static uint32_t cum_bytes;
static frame_data ref_frame;
static frame_data prev_dis_frame;
static frame_data prev_cap_frame;

static bool perform_two_pass_analysis;
static uint32_t epan_auto_reset_count;
static bool epan_auto_reset;

static uint32_t selected_frame_number;

/*
 * The way the packet decode is to be written.
 */
typedef enum {
    WRITE_NONE,     /* dummy initial state */
    WRITE_TEXT,     /* summary or detail text */
    WRITE_XML,      /* PDML or PSML */
    WRITE_FIELDS,   /* User defined list of fields */
    WRITE_JSON,     /* JSON */
    WRITE_JSON_RAW, /* JSON only raw hex */
    WRITE_EK        /* JSON bulk insert to Elasticsearch */
        /* Add CSV and the like here */
} output_action_e;

static output_action_e output_action;
static bool do_dissection;     /* true if we have to dissect each packet */
static bool print_packet_info; /* true if we're to print packet information */
static bool print_summary;     /* true if we're to print packet summary information */
static bool print_details;     /* true if we're to print packet details information */
static bool print_hex;         /* true if we're to print hex/ascii information */
static bool line_buffered;
static bool quiet;
static bool really_quiet;
static char* delimiter_char = " ";
static bool dissect_color;
static unsigned hexdump_source_option = HEXDUMP_SOURCE_MULTI; /* Default - Enable legacy multi-source mode */
static unsigned hexdump_ascii_option = HEXDUMP_ASCII_INCLUDE; /* Default - Enable legacy undelimited ASCII dump */
static unsigned hexdump_timestamp_option = HEXDUMP_TIMESTAMP_NONE; /* Default - no timestamp preamble */

static print_format_e print_format = PR_FMT_TEXT;
static print_stream_t *print_stream;

static char *output_file_name;

static output_fields_t* output_fields;

static bool no_duplicate_keys;
static proto_node_children_grouper_func node_children_grouper = proto_node_group_children_by_unique;

static json_dumper jdumper;

/* The line separator used between packets, changeable via the -S option */
static const char *separator = "";

/* Per-file comments to be added to the output file. */
static GPtrArray *capture_comments;

static bool prefs_loaded;

static void reset_epan_mem(capture_file *cf, epan_dissect_t *edt, bool tree, bool visual);

typedef enum {
    PROCESS_FILE_SUCCEEDED,
    PROCESS_FILE_NO_FILE_PROCESSED,
    PROCESS_FILE_ERROR,
    PROCESS_FILE_INTERRUPTED
} process_file_status_t;
static process_file_status_t process_cap_file(capture_file *, char *, int, bool, int, int64_t, int, ws_compression_type);

static bool process_packet_single_pass(capture_file *cf,
        epan_dissect_t *edt, int64_t offset, wtap_rec *rec, unsigned tap_flags);
static void show_print_file_io_error(void);
static bool write_preamble(capture_file *cf);
static bool print_packet(capture_file *cf, epan_dissect_t *edt);
static bool write_finale(void);

static GHashTable *output_only_tables;

static bool opt_print_timers;
struct elapsed_pass_s {
    int64_t dissect;
    int64_t dfilter_read;
    int64_t dfilter_filter;
};
static struct {
    int64_t                dfilter_expand;
    int64_t                dfilter_compile;
    struct elapsed_pass_s  first_pass;
    int64_t                elapsed_first_pass;
    struct elapsed_pass_s  second_pass;
    int64_t                elapsed_second_pass;
}
strato_elapsed;

static void
print_elapsed_json(const char *cf_name, const char *dfilter)
{
    json_dumper dumper = {
        .output_file = stderr,
        .flags = JSON_DUMPER_FLAGS_PRETTY_PRINT,
    };

    if (strato_elapsed.elapsed_first_pass == 0) {
        // Should not happen
        ws_warning("Print timers requested but no timing info provided");
        return;
    }

#define DUMP(name, val) \
        json_dumper_set_member_name(&dumper, name); \
        json_dumper_value_anyf(&dumper, "%"PRId64, val)

    json_dumper_begin_object(&dumper);
    json_dumper_set_member_name(&dumper, "version");
    json_dumper_value_string(&dumper, get_ws_vcs_version_info_short());
    if (cf_name) {
        json_dumper_set_member_name(&dumper, "path");
        json_dumper_value_string(&dumper, cf_name);
    }
    if (dfilter) {
        json_dumper_set_member_name(&dumper, "filter");
        json_dumper_value_string(&dumper, dfilter);
    }
    json_dumper_set_member_name(&dumper, "time_unit");
    json_dumper_value_string(&dumper, "microseconds");
    DUMP("elapsed", strato_elapsed.elapsed_first_pass +
        strato_elapsed.elapsed_second_pass);
    DUMP("dfilter_expand", strato_elapsed.dfilter_expand);
    DUMP("dfilter_compile", strato_elapsed.dfilter_compile);
    json_dumper_begin_array(&dumper);
    json_dumper_begin_object(&dumper);
    DUMP("elapsed", strato_elapsed.elapsed_first_pass);
    DUMP("dissect", strato_elapsed.first_pass.dissect);
    DUMP("display_filter", strato_elapsed.first_pass.dfilter_filter);
    DUMP("read_filter", strato_elapsed.first_pass.dfilter_read);
    json_dumper_end_object(&dumper);
    if (strato_elapsed.elapsed_second_pass) {
        json_dumper_begin_object(&dumper);
        DUMP("elapsed", strato_elapsed.elapsed_second_pass);
        DUMP("dissect", strato_elapsed.second_pass.dissect);
        DUMP("display_filter", strato_elapsed.second_pass.dfilter_filter);
        DUMP("read_filter", strato_elapsed.second_pass.dfilter_read);
        json_dumper_end_object(&dumper);
    }
    json_dumper_end_array(&dumper);
    json_dumper_end_object(&dumper);
    json_dumper_finish(&dumper);
}

static void
list_capture_types(void)
{
    GArray *writable_type_subtypes;

    fprintf(stderr, "strato: The available capture file types for the \"-F\" flag are:\n");
    writable_type_subtypes = wtap_get_writable_file_types_subtypes(FT_SORT_BY_NAME);
    for (unsigned i = 0; i < writable_type_subtypes->len; i++) {
        int ft = g_array_index(writable_type_subtypes, int, i);
        fprintf(stderr, "    %s - %s\n", wtap_file_type_subtype_name(ft),
                wtap_file_type_subtype_description(ft));
    }
    g_array_free(writable_type_subtypes, TRUE);
}

static void
list_output_compression_types(void) {
    GSList *output_compression_types;

    cmdarg_err("The available output compression type(s) are:");
    output_compression_types = ws_get_all_output_compression_type_names_list();
    for (GSList *compression_type = output_compression_types;
        compression_type != NULL;
        compression_type = g_slist_next(compression_type)) {
            cmdarg_err_cont("   %s", (const char *)compression_type->data);
        }

    g_slist_free(output_compression_types);
}

struct string_elem {
    const char *sstr;   /* The short string */
    const char *lstr;   /* The long string */
};

static int
string_compare(const void *a, const void *b)
{
    return strcmp(((const struct string_elem *)a)->sstr,
            ((const struct string_elem *)b)->sstr);
}

static void
string_elem_print(void *data)
{
    fprintf(stderr, "    %s - %s\n",
            ((struct string_elem *)data)->sstr,
            ((struct string_elem *)data)->lstr);
}

static void
list_read_capture_types(void)
{
    unsigned            i;
    size_t              num_file_types;
    struct string_elem *captypes;
    GSList             *list = NULL;
    const char *magic = "Magic-value-based";
    const char *heuristic = "Heuristics-based";

    /* How many readable file types are there? */
    num_file_types = 0;
    for (i = 0; open_routines[i].name != NULL; i++)
        num_file_types++;
    captypes = g_new(struct string_elem, num_file_types);

    fprintf(stderr, "strato: The available read file types for the \"-X read_format:\" option are:\n");
    for (i = 0; i < num_file_types && open_routines[i].name != NULL; i++) {
        captypes[i].sstr = open_routines[i].name;
        captypes[i].lstr = (open_routines[i].type == OPEN_INFO_MAGIC) ? magic : heuristic;
        list = g_slist_insert_sorted(list, &captypes[i], string_compare);
    }
    g_slist_free_full(list, string_elem_print);
    g_free(captypes);
}

static void
list_export_pdu_taps(void)
{
    fprintf(stderr, "strato: The available export tap names and the encapsulation types they produce for the \"-U tap_name\" option are:\n");
    for (GSList *export_pdu_tap_name_list = get_export_pdu_tap_list();
            export_pdu_tap_name_list != NULL;
            export_pdu_tap_name_list = g_slist_next(export_pdu_tap_name_list)) {
        fprintf(stderr, "    %s - %s\n", (const char*)(export_pdu_tap_name_list->data), wtap_encap_description(export_pdu_tap_get_encap((const char*)export_pdu_tap_name_list->data)));
    }
}

static void
print_usage(FILE *output)
{
    fprintf(output, "\n");
    fprintf(output, "Usage: strato [options] ...\n");
    fprintf(output, "\n");

    /*fprintf(output, "\n");*/
    fprintf(output, "Input file:\n");
    fprintf(output, "  -r <infile>, --read-file <infile>\n");
    fprintf(output, "                           set the filename to read from (or '-' for stdin)\n");

    fprintf(output, "\n");
    fprintf(output, "Processing:\n");
    fprintf(output, "  -2                       perform a two-pass analysis\n");
    fprintf(output, "  -M <packet count>        perform session auto reset\n");
    fprintf(output, "  -R <read filter>, --read-filter <read filter>\n");
    fprintf(output, "                           packet Read filter in Wireshark display filter syntax\n");
    fprintf(output, "                           (requires -2)\n");
    fprintf(output, "  -Y <display filter>, --display-filter <display filter>\n");
    fprintf(output, "                           packet displaY filter in Wireshark display filter\n");
    fprintf(output, "                           syntax\n");
    fprintf(output, "  -n                       disable all name resolutions (def: \"mNd\" enabled, or\n");
    fprintf(output, "                           as set in preferences)\n");
    // Note: the order of the flags here matches the options in the settings dialog e.g. "dsN" only have an effect if "n" is set
    fprintf(output, "  -N <name resolve flags>  enable specific name resolution(s): \"mtndsNvg\"\n");
    fprintf(output, "  -d %s ...\n", DECODE_AS_ARG_TEMPLATE);
    fprintf(output, "                           \"Decode As\", see the man page for details\n");
    fprintf(output, "                           Example: tcp.port==8888,http\n");
    fprintf(output, "  -H <hosts file>          read a list of entries from a hosts file, which will\n");
    fprintf(output, "                           then be written to a capture file. (Implies -W n)\n");
    fprintf(output, "  --enable-protocol <proto_name>\n");
    fprintf(output, "                           enable dissection of proto_name\n");
    fprintf(output, "  --disable-protocol <proto_name>\n");
    fprintf(output, "                           disable dissection of proto_name\n");
    fprintf(output, "  --only-protocols <protocols>\n");
    fprintf(output, "                           Only enable dissection of these protocols, comma\n");
    fprintf(output, "                           separated. Disable everything else\n");
    fprintf(output, "  --disable-all-protocols\n");
    fprintf(output, "                           Disable dissection of all protocols\n");
    fprintf(output, "  --enable-heuristic <short_name>\n");
    fprintf(output, "                           enable dissection of heuristic protocol\n");
    fprintf(output, "  --disable-heuristic <short_name>\n");
    fprintf(output, "                           disable dissection of heuristic protocol\n");

    /*fprintf(output, "\n");*/
    fprintf(output, "Output:\n");
    fprintf(output, "  -w <outfile|->           write packets to a pcapng-format file named \"outfile\"\n");
    fprintf(output, "                           (or '-' for stdout). If the output filename has the\n");
    fprintf(output, "                           .gz extension, it will be compressed to a gzip archive\n");
    fprintf(output, "  --capture-comment <comment>\n");
    fprintf(output, "                           add a capture file comment, if supported\n");
    fprintf(output, "  -C <config profile>      start with specified configuration profile\n");
    fprintf(output, "  --global-profile         use the global profile instead of personal profile\n");
    fprintf(output, "  -F <output file type>    set the output file type; default is pcapng.\n");
    fprintf(output, "                           an empty \"-F\" option will list the file types\n");
    fprintf(output, "  -V                       add output of packet tree        (Packet Details)\n");
    fprintf(output, "  -O <protocols>           Only show packet details of these protocols, comma\n");
    fprintf(output, "                           separated\n");
    fprintf(output, "  -P, --print              print packet summary even when writing to a file\n");
    fprintf(output, "  -S <separator>           the line separator to print between packets\n");
    fprintf(output, "  -x                       add output of hex and ASCII dump (Packet Bytes)\n");
    fprintf(output, "  --hexdump <hexoption>    add hexdump, set options for data source and ASCII dump\n");
    fprintf(output, "     all                   dump all data sources (-x default)\n");
    fprintf(output, "     frames                dump only frame data source\n");
    fprintf(output, "     ascii                 include ASCII dump text (-x default)\n");
    fprintf(output, "     delimit               delimit ASCII dump text with '|' characters\n");
    fprintf(output, "     noascii               exclude ASCII dump text\n");
    fprintf(output, "     time                  include frame timestamp preamble\n");
    fprintf(output, "     notime                do not include frame timestamp preamble (-x default)\n");
    fprintf(output, "     help                  display help for --hexdump and exit\n");
    fprintf(output, "  -T pdml|ps|psml|json|jsonraw|ek|tabs|text|fields|?\n");
    fprintf(output, "                           format of text output (def: text)\n");
    fprintf(output, "  -j <protocolfilter>      protocols layers filter if -T ek|pdml|json selected\n");
    fprintf(output, "                           (e.g. \"ip ip.flags text\", filter does not expand child\n");
    fprintf(output, "                           nodes, unless child is specified also in the filter)\n");
    fprintf(output, "  -J <protocolfilter>      top level protocol filter if -T ek|pdml|json selected\n");
    fprintf(output, "                           (e.g. \"http tcp\", filter which expands all child nodes)\n");
    fprintf(output, "  -e <field>               field to print if -Tfields selected (e.g. tcp.port,\n");
    fprintf(output, "                           _ws.col.info)\n");
    fprintf(output, "                           this option can be repeated to print multiple fields\n");
    fprintf(output, "  -E<fieldsoption>=<value> set options for output when -Tfields selected:\n");
    fprintf(output, "     bom=y|n               print a UTF-8 BOM\n");
    fprintf(output, "     header=y|n            switch headers on and off\n");
    fprintf(output, "     separator=/t|/s|<char> select tab, space, printable character as separator\n");
    fprintf(output, "     occurrence=f|l|a      print first, last or all occurrences of each field\n");
    fprintf(output, "     aggregator=,|/s|<char> select comma, space, printable character as\n");
    fprintf(output, "                           aggregator\n");
    fprintf(output, "     quote=d|s|n           select double, single, no quotes for values\n");
    fprintf(output, "  -t (a|ad|adoy|d|dd|e|r|u|ud|udoy)[.[N]]|.[N]\n");
    fprintf(output, "                           output format of time stamps (def: r: rel. to first)\n");
    fprintf(output, "  -u s|hms                 output format of seconds (def: s: seconds)\n");
    fprintf(output, "  -l                       flush standard output after each packet\n");
    fprintf(output, "                           (implies --update-interval 0)\n");
    fprintf(output, "  -q                       be more quiet on stdout (e.g. when using statistics)\n");
    fprintf(output, "  -Q                       only log true errors to stderr (quieter than -q)\n");
    fprintf(output, "  -g                       enable group read access on the output file(s)\n");
    fprintf(output, "  -W n                     Save extra information in the file, if supported.\n");
    fprintf(output, "                           n = write network address resolution information\n");
    fprintf(output, "  -X <key>:<value>         eXtension options, see the man page for details\n");
    fprintf(output, "  -U tap_name              PDUs export mode, see the man page for details\n");
    fprintf(output, "  -z <statistics>          various statistics, see the man page for details\n");
    fprintf(output, "  --export-objects <protocol>,<destdir>\n");
    fprintf(output, "                           save exported objects for a protocol to a directory\n");
    fprintf(output, "                           named \"destdir\"\n");
    fprintf(output, "  --export-tls-session-keys <keyfile>\n");
    fprintf(output, "                           export TLS Session Keys to a file named \"keyfile\"\n");
    fprintf(output, "  --color                  color output text similarly to the Wireshark GUI,\n");
    fprintf(output, "                           requires a terminal with 24-bit color support\n");
    fprintf(output, "                           Also supplies color attributes to pdml and psml formats\n");
    fprintf(output, "                           (Note that attributes are nonstandard)\n");
    fprintf(output, "  --no-duplicate-keys      If -T json is specified, merge duplicate keys in an object\n");
    fprintf(output, "                           into a single key with as value a json array containing all\n");
    fprintf(output, "                           values\n");
    fprintf(output, "  --elastic-mapping-filter <protocols> If -G elastic-mapping is specified, put only the\n");
    fprintf(output, "                           specified protocols within the mapping file\n");
    fprintf(output, "  --temp-dir <directory>   write temporary files to this directory\n");
    fprintf(output, "                           (default: %s)\n", g_get_tmp_dir());
    fprintf(output, "  --compress <type>        compress the output file using the type compression format\n");
    fprintf(output, "\n");

    ws_log_print_usage(output);
    fprintf(output, "\n");

    fprintf(output, "Miscellaneous:\n");
    fprintf(output, "  -h, --help               display this help and exit\n");
    fprintf(output, "  -v, --version            display version info and exit\n");
    fprintf(output, "  -o <name>:<value> ...    override preference setting\n");
    fprintf(output, "  -K <keytab>              keytab file to use for kerberos decryption\n");
    fprintf(output, "  -G [report]              dump one of several available reports and exit\n");
    fprintf(output, "                           default report=\"fields\"\n");
    fprintf(output, "                           use \"-G help\" for more help\n");
#ifdef __linux__
    fprintf(output, "\n");
    fprintf(output, "Dumpcap can benefit from an enabled BPF JIT compiler if available.\n");
    fprintf(output, "You might want to enable it by executing:\n");
    fprintf(output, " \"echo 1 > /proc/sys/net/core/bpf_jit_enable\"\n");
    fprintf(output, "Note that this can make your system less secure!\n");
#endif

}

static void
glossary_option_help(void)
{
    FILE *output;

    output = stdout;

    fprintf(output, "%s\n", get_appname_and_version());

    fprintf(output, "\n");
    fprintf(output, "Usage: strato -G <report>\n");
    fprintf(output, "\n");
    fprintf(output, "Glossary table reports:\n");
    fprintf(output, "  -G column-formats        dump column format codes and exit\n");
    fprintf(output, "  -G decodes               dump \"layer type\"/\"decode as\" associations and exit\n");
    fprintf(output, "  -G dissector-tables      dump dissector table names, types, and properties\n");
    fprintf(output, "  -G dissectors            dump registered dissector names\n");
    fprintf(output, "  -G elastic-mapping       dump ElasticSearch mapping file\n");
    fprintf(output, "  -G enterprises           dump IANA Private Enterprise Number (PEN) table\n");
    fprintf(output, "  -G fieldcount            dump count of header fields and exit\n");
    fprintf(output, "  -G fields,[prefix]       dump fields glossary and exit\n");
    fprintf(output, "  -G ftypes                dump field type basic and descriptive names\n");
    fprintf(output, "  -G heuristic-decodes     dump heuristic dissector tables\n");
    fprintf(output, "  -G manuf                 dump ethernet manufacturer tables\n");
    fprintf(output, "  -G plugins               dump installed plugins and exit\n");
    fprintf(output, "  -G protocols             dump protocols in registration database and exit\n");
    fprintf(output, "  -G services              dump transport service (port) names\n");
    fprintf(output, "  -G values                dump value, range, true/false strings and exit\n");
    fprintf(output, "\n");
    fprintf(output, "Preference reports:\n");
    fprintf(output, "  -G currentprefs          dump current preferences and exit\n");
    fprintf(output, "  -G defaultprefs          dump default preferences and exit\n");
    fprintf(output, "  -G folders               dump about:folders\n");
    fprintf(output, "\n");
}

static void
hexdump_option_help(FILE *output)
{
    fprintf(output, "%s\n", get_appname_and_version());
    fprintf(output, "\n");
    fprintf(output, "strato: Valid --hexdump <hexoption> values include:\n");
    fprintf(output, "\n");
    fprintf(output, "Data source options:\n");
    fprintf(output, "  all                      add hexdump, dump all data sources (-x default)\n");
    fprintf(output, "  frames                   add hexdump, dump only frame data source\n");
    fprintf(output, "\n");
    fprintf(output, "ASCII options:\n");
    fprintf(output, "  ascii                    add hexdump, include ASCII dump text (-x default)\n");
    fprintf(output, "  delimit                  add hexdump, delimit ASCII dump text with '|' characters\n");
    fprintf(output, "  noascii                  add hexdump, exclude ASCII dump text\n");
    fprintf(output, "\n");
    fprintf(output, "Timestamp options:\n");
    fprintf(output, "  time                     add hexdump, include frame timestamp preamble (uses the format from -t)\n");
    fprintf(output, "  notime                   add hexdump, do not include frame timestamp preamble (-x default)\n");
    fprintf(output, "Miscellaneous:\n");
    fprintf(output, "  help                     display this help and exit\n");
    fprintf(output, "\n");
    fprintf(output, "Example:\n");
    fprintf(output, "\n");
    fprintf(output, "    $ strato ... --hexdump frames --hexdump delimit ...\n");
    fprintf(output, "\n");
}

static void
print_current_user(void)
{
    char *cur_user, *cur_group;

    if (started_with_special_privs()) {
        cur_user = get_cur_username();
        cur_group = get_cur_groupname();
        fprintf(stderr, "Running as user \"%s\" and group \"%s\".",
                cur_user, cur_group);
        g_free(cur_user);
        g_free(cur_group);
        if (running_with_special_privs()) {
            fprintf(stderr, " This could be dangerous.");
        }
        fprintf(stderr, "\n");
    }
}

static void
gather_strato_compile_info(feature_list l)
{
    /* Capture libraries */
    gather_caplibs_compile_info(l);
    epan_gather_compile_info(l);
}

static void
gather_strato_runtime_info(feature_list l)
{
    /* stuff used by libwireshark */
    epan_gather_runtime_info(l);
}

static bool
_compile_dfilter(const char *text, dfilter_t **dfp, const char *caller)
{
    bool ok;
    df_error_t *df_err;
    char *err_off;
    char *expanded;
    int64_t elapsed_start;

    elapsed_start = g_get_monotonic_time();
    expanded = dfilter_expand(text, &df_err);
    if (expanded == NULL) {
        cmdarg_err("%s", df_err->msg);
        df_error_free(&df_err);
        return false;
    }
    strato_elapsed.dfilter_expand = g_get_monotonic_time() - elapsed_start;

    elapsed_start = g_get_monotonic_time();
    ok = dfilter_compile_full(expanded, dfp, &df_err, DF_OPTIMIZE, caller);
    if (!ok ) {
        cmdarg_err("%s", df_err->msg);

        if (df_err->loc.col_start >= 0) {
            err_off = ws_strdup_underline(NULL, df_err->loc.col_start, df_err->loc.col_len);
            cmdarg_err_cont("    %s", expanded);
            cmdarg_err_cont("    %s", err_off);
            g_free(err_off);
        }
        df_error_free(&df_err);
    }
    strato_elapsed.dfilter_compile = g_get_monotonic_time() - elapsed_start;

    g_free(expanded);
    return ok;
}

#define compile_dfilter(text, dfp) _compile_dfilter(text, dfp, __func__)

static bool
protocolfilter_add_opt(const char* arg, pf_flags filter_flags)
{
    char **newfilter = NULL;
    for (newfilter = wmem_strsplit(wmem_epan_scope(), arg, " ", -1); *newfilter; newfilter++) {
        if (strcmp(*newfilter, "") == 0) {
            /* Don't treat the empty string as an intended field abbreviation
             * to output, consecutive spaces on the command line probably
             * aren't intentional.
             */
            continue;
        }
        if (!output_fields_add_protocolfilter(output_fields, *newfilter, filter_flags)) {
            cmdarg_err("%s was already specified with different filter flags. Overwriting previous protocol filter.", *newfilter);
        }
    }
    return true;
}

static void
about_folders(void)
{
    const char           *constpath;
    char                 *path;
    int                   i;
    char                **resultArray;

    /* "file open" */

    /*
     * Fetching the "File" dialogs folder not implemented.
     * This is arguably just a pwd for a ui/cli .
     */

    /* temp */
    constpath = g_get_tmp_dir();
    printf("%-21s\t%s\n", "Temp:", constpath);

    /* pers conf */
    path = get_persconffile_path("", false);
    printf("%-21s\t%s\n", "Personal configuration:", path);
    g_free(path);

    /* global conf */
    constpath = get_datafile_dir();
    if (constpath != NULL) {
        printf("%-21s\t%s\n", "Global configuration:", constpath);
    }

    /* system */
    constpath = get_systemfile_dir();
    printf("%-21s\t%s\n", "System:", constpath);

    /* program */
    constpath = get_progfile_dir();
    printf("%-21s\t%s\n", "Program:", constpath);

#ifdef HAVE_PLUGINS
    /* pers plugins */
    printf("%-21s\t%s\n", "Personal Plugins:", get_plugins_pers_dir_with_version());

    /* global plugins */
    printf("%-21s\t%s\n", "Global Plugins:", get_plugins_dir_with_version());
#endif

#ifdef HAVE_LUA
    /* pers lua plugins */
    printf("%-21s\t%s\n", "Personal Lua Plugins:", get_plugins_pers_dir());

    /* global lua plugins */
    printf("%-21s\t%s\n", "Global Lua Plugins:", get_plugins_dir());
#endif

    /* Personal Extcap */
    constpath = get_extcap_pers_dir();

    resultArray = g_strsplit(constpath, G_SEARCHPATH_SEPARATOR_S, 10);
    for(i = 0; resultArray[i]; i++)
        printf("%-21s\t%s\n", "Personal Extcap path:", g_strstrip(resultArray[i]));

    g_strfreev(resultArray);

    /* Global Extcap */
    constpath = get_extcap_dir();

    resultArray = g_strsplit(constpath, G_SEARCHPATH_SEPARATOR_S, 10);
    for(i = 0; resultArray[i]; i++)
        printf("%-21s\t%s\n", "Global Extcap path:", g_strstrip(resultArray[i]));

    g_strfreev(resultArray);

    /* MaxMindDB */
    path = maxmind_db_get_paths();

    resultArray = g_strsplit(path, G_SEARCHPATH_SEPARATOR_S, 10);

    for(i = 0; resultArray[i]; i++)
        printf("%-21s\t%s\n", "MaxMind database path:", g_strstrip(resultArray[i]));

    g_strfreev(resultArray);
    g_free(path);

#ifdef HAVE_LIBSMI
    /* SMI MIBs/PIBs */
    path = oid_get_default_mib_path();

    resultArray = g_strsplit(path, G_SEARCHPATH_SEPARATOR_S, 20);

    for(i = 0; resultArray[i]; i++)
        printf("%-21s\t%s\n", "MIB/PIB path:", g_strstrip(resultArray[i]));

    g_strfreev(resultArray);
    g_free(path);
#endif

}

static int
dump_glossary(const char* glossary, const char* elastic_mapping_filter)
{
    int exit_status = EXIT_SUCCESS;
    /* If invoked with the "-G" flag, we dump out information based on
       the argument to the "-G" flag.
     */

    /* This is now called after the preferences are loaded and all
     * the command line options are handled, including -o, -d,
     * --[enable|disable]-[protocol|heuristic].
     * Some UATs can register new fields (e.g. HTTP/2), so for most
     * cases load everything.
     *
     * prefs_reset() is used for defaultprefs to get the default values.
     * Note that makes it difficult to use defaultprefs in concert with
     * any other glossary (we could do it last.)
     */
    proto_initialize_all_prefixes();

    if (strcmp(glossary, "column-formats") == 0)
        column_dump_column_formats();
    else if (strcmp(glossary, "currentprefs") == 0) {
        write_prefs(NULL);
    }
    else if (strcmp(glossary, "decodes") == 0) {
        dissector_dump_decodes();
    } else if (strcmp(glossary, "defaultprefs") == 0) {
        prefs_reset();
        write_prefs(NULL);
    } else if (strcmp(glossary, "dissector-tables") == 0)
        dissector_dump_dissector_tables();
    else if (strcmp(glossary, "dissectors") == 0)
        dissector_dump_dissectors();
    else if (strcmp(glossary, "elastic-mapping") == 0)
        proto_registrar_dump_elastic(elastic_mapping_filter);
    else if (strncmp(glossary, "elastic-mapping,", strlen("elastic-mapping,")) == 0) {
        elastic_mapping_filter = glossary + strlen("elastic-mapping,");
        proto_registrar_dump_elastic(elastic_mapping_filter);
    }
    else if (strcmp(glossary, "fieldcount") == 0) {
        /* return value for the test suite */
        exit_status = proto_registrar_dump_fieldcount();
    }
    else if (strcmp(glossary, "fields") == 0) {
        proto_registrar_dump_fields();
    }
    else if (strncmp(glossary, "fields,", strlen("fields,")) == 0) {
        const char* prefix = glossary + strlen("fields,");
        bool matched = proto_registrar_dump_field_completions(prefix);
        if (!matched) {
            cmdarg_err("No field or protocol begins with \"%s\"", prefix);
            exit_status = EXIT_FAILURE;
        }
    }
    else if (strcmp(glossary, "folders") == 0) {
        about_folders();
    } else if (strcmp(glossary, "ftypes") == 0)
        proto_registrar_dump_ftypes();
    else if (strcmp(glossary, "heuristic-decodes") == 0) {
        dissector_dump_heur_decodes();
    } else if (strcmp(glossary, "manuf") == 0)
        ws_manuf_dump(stdout);
    else if (strcmp(glossary, "enterprises") == 0)
        global_enterprises_dump(stdout);
    else if (strcmp(glossary, "services") == 0)
        global_services_dump(stdout);
    else if (strcmp(glossary, "plugins") == 0) {
#ifdef HAVE_PLUGINS
        codecs_init();
        plugins_dump_all();
#endif
#ifdef HAVE_LUA
        wslua_plugins_dump_all();
#endif
        extcap_dump_all();
    }
    else if (strcmp(glossary, "protocols") == 0) {
        proto_registrar_dump_protocols();
    } else if (strcmp(glossary, "values") == 0)
        proto_registrar_dump_values();
    else if (strcmp(glossary, "help") == 0)
        glossary_option_help();
    /* These are supported only for backwards compatibility and may or may not work
     * for a given user in a given directory on a given operating system with a given
     * command-line interpreter.
     */
    else if (strcmp(glossary, "?") == 0)
        glossary_option_help();
    else if (strcmp(glossary, "-?") == 0)
        glossary_option_help();
    else {
        cmdarg_err("Invalid \"%s\" option for -G flag, enter -G help for more help.", glossary);
        exit_status = WS_EXIT_INVALID_OPTION;
    }

    return exit_status;
}

static bool
must_do_dissection(dfilter_t *rfcode, dfilter_t *dfcode,
        char *volatile pdu_export_arg)
{
    /* We have to dissect each packet if:

       we're printing information about each packet;

       we're using a read filter on the packets;

       we're using a display filter on the packets;

       we're exporting PDUs;

       we're using any taps that need dissection. */
    return print_packet_info || rfcode || dfcode || pdu_export_arg ||
        tap_listeners_require_dissection();
}

int
main(int argc, char *argv[])
{
    char                *err_msg;
    int                  opt;
    static const struct ws_option long_options[] = {
        {"help", ws_no_argument, NULL, 'h'},
        {"version", ws_no_argument, NULL, 'v'},
        LONGOPT_CAPTURE_COMMON
        LONGOPT_DISSECT_COMMON
        LONGOPT_READ_CAPTURE_COMMON
        LONGOPT_WSLOG
        {"print", ws_no_argument, NULL, 'P'},
        {"export-objects", ws_required_argument, NULL, LONGOPT_EXPORT_OBJECTS},
        {"export-tls-session-keys", ws_required_argument, NULL, LONGOPT_EXPORT_TLS_SESSION_KEYS},
        {"color", ws_no_argument, NULL, LONGOPT_COLOR},
        {"no-duplicate-keys", ws_no_argument, NULL, LONGOPT_NO_DUPLICATE_KEYS},
        {"elastic-mapping-filter", ws_required_argument, NULL, LONGOPT_ELASTIC_MAPPING_FILTER},
        {"capture-comment", ws_required_argument, NULL, LONGOPT_CAPTURE_COMMENT},
        {"hexdump", ws_required_argument, NULL, LONGOPT_HEXDUMP},
        {"selected-frame", ws_required_argument, NULL, LONGOPT_SELECTED_FRAME},
        {"print-timers", ws_no_argument, NULL, LONGOPT_PRINT_TIMERS},
        {"global-profile", ws_no_argument, NULL, LONGOPT_GLOBAL_PROFILE},
        {"compress", ws_required_argument, NULL, LONGOPT_COMPRESS},
        {0, 0, 0, 0}
    };
    bool                 arg_error = false;
    bool                 has_extcap_options = false;
    volatile bool        is_capturing = true;

    int                  err;
    char                *err_info;
    bool                 exp_pdu_status;
    volatile process_file_status_t status;
    volatile bool        draw_taps = false;
    volatile int         exit_status = EXIT_SUCCESS;
    bool                 capture_option_specified = false;
    int                  max_packet_count = 0;
    volatile int          out_file_type = WTAP_FILE_TYPE_SUBTYPE_UNKNOWN;
    volatile bool         out_file_name_res = false;
    volatile int          in_file_type = WTAP_TYPE_AUTO;
    char                 *volatile cf_name = NULL;
    char                 *rfilter = NULL;
    char                 *volatile dfilter = NULL;
    dfilter_t            *rfcode = NULL;
    dfilter_t            *dfcode = NULL;
    e_prefs              *prefs_p;
    char                 *output_only = NULL;
    char                 *volatile pdu_export_arg = NULL;
    char                 *volatile exp_pdu_filename = NULL;
    const char           *volatile tls_session_keys_file = NULL;
    exp_pdu_t             exp_pdu_tap_data;
    const char*           glossary = NULL;
    const char*           elastic_mapping_filter = NULL;
    ws_compression_type   volatile compression_type = WS_FILE_UNKNOWN_COMPRESSION;

    /*
     * The leading + ensures that getopt_long() does not permute the argv[]
     * entries.
     *
     * We have to make sure that the first getopt_long() preserves the content
     * of argv[] for the subsequent getopt_long() call.
     *
     * We use getopt_long() in both cases to ensure that we're using a routine
     * whose permutation behavior we can control in the same fashion on all
     * platforms, and so that, if we ever need to process a long argument before
     * doing further initialization, we can do so.
     *
     * Glibc and Solaris libc document that a leading + disables permutation
     * of options, regardless of whether POSIXLY_CORRECT is set or not; *BSD
     * and macOS don't document it, but do so anyway.
     *
     * We do *not* use a leading - because the behavior of a leading - is
     * platform-dependent.
     */
#define OPTSTRING "+2" OPTSTRING_CAPTURE_COMMON OPTSTRING_DISSECT_COMMON OPTSTRING_READ_CAPTURE_COMMON "M:C:e:E:F:gG:hH:j:J:lo:O:PqQS:T:U:vVw:W:xX:z:"

    static const char    optstring[] = OPTSTRING;

    /* Set the program name. */
    g_set_prgname("strato");

    /*
     * Set the C-language locale to the native environment and set the
     * code page to UTF-8 on Windows.
     */
#ifdef _WIN32
    setlocale(LC_ALL, ".UTF-8");
#else
    setlocale(LC_ALL, "");
#endif

    ws_tzset();

    cmdarg_err_init(stderr_cmdarg_err, stderr_cmdarg_err_cont);

    /* Initialize log handler early so we can have proper logging during startup. */
    ws_log_init(vcmdarg_err);

    /* Early logging command-line initialization. */
    ws_log_parse_args(&argc, argv, optstring, long_options, vcmdarg_err, WS_EXIT_INVALID_OPTION);

    ws_noisy("Finished log init and parsing command line log arguments");
    ws_debug("strato started with %d args", argc);

#ifdef _WIN32
    create_app_running_mutex();
#endif /* _WIN32 */

    /*
     * Get credential information for later use, and drop privileges
     * before doing anything else.
     * Let the user know if anything happened.
     */
    init_process_policies();
    relinquish_special_privs_perm();
    print_current_user();

    /*
     * Attempt to get the pathname of the directory containing the
     * executable file.
     */
    set_application_flavor(APPLICATION_FLAVOR_STRATOSHARK);
    err_msg = configuration_init(argv[0]);
    if (err_msg != NULL) {
        fprintf(stderr,
                "strato: Can't get pathname of directory containing the strato program: %s.\n"
                "It won't be possible to capture traffic.\n"
                "Report this to the Wireshark developers.",
                err_msg);
        g_free(err_msg);
    }

    initialize_funnel_ops();

#ifdef _WIN32
    ws_init_dll_search_path();
#endif /* _WIN32 */

    /* Initialize the version information. */
    ws_init_version_info("strato",
            gather_strato_compile_info, gather_strato_runtime_info);

    /* Fail sometimes. Useful for testing fuzz scripts. */
    /* if (g_random_int_range(0, 100) < 5) abort(); */

    /*
     * In order to have the -X opts assigned before the wslua machine starts
     * we need to call getopt_long before epan_init() gets called.
     *
     * In order to handle, for example, -o options, we also need to call it
     * *after* epan_init() gets called, so that the dissectors have had a
     * chance to register their preferences.
     *
     * Spawning a bunch of extcap processes can delay program startup,
     * particularly on Windows. Check to see if we have any options that
     * might require extcap and set has_extcap_options = true if that's
     * the case.
     *
     * XXX - can we do this all with one getopt_long() call, saving the
     * arguments we can't handle until after initializing libwireshark,
     * and then process them after initializing libwireshark?
     *
     * We set ws_opterr to 0 so that ws_getopt_long doesn't print error
     * messages for bad long options. We'll do that once, in the final
     * call where all the error handling happens.
     */
    ws_opterr = 0;

    /*  We should check at first if we should use a global profile before
        parsing the profile name
        XXX - We could check this in the next ws_getopt_long, and save the
        profile name and only apply it after finishing the loop.  */
    while ((opt = ws_getopt_long(argc, argv, optstring, long_options, NULL)) != -1) {
        switch (opt) {
            case LONGOPT_GLOBAL_PROFILE:
                    set_persconffile_dir(get_datafile_dir());
                    break;
            default:
                break;
        }
    }

    /*
     * Reset the options parser, set ws_optreset to 1 and set ws_optind to 1.
     * We still don't want to print error messages, though.
     */
    ws_optreset = 1;
    ws_optind = 1;

    while ((opt = ws_getopt_long(argc, argv, optstring, long_options, NULL)) != -1) {
        switch (opt) {
            case 'C':        /* Configuration Profile */
                if (profile_exists (ws_optarg, false)) {
                    set_profile_name (ws_optarg);
                } else if (profile_exists (ws_optarg, true)) {
                    char  *pf_dir_path, *pf_dir_path2, *pf_filename;
                    /* Copy from global profile */
                    if (create_persconffile_profile(ws_optarg, &pf_dir_path) == -1) {
                        cmdarg_err("Can't create directory\n\"%s\":\n%s.",
                            pf_dir_path, g_strerror(errno));

                        g_free(pf_dir_path);
                        exit_status = WS_EXIT_INVALID_FILE;
                        goto clean_exit;
                    }
                    if (copy_persconffile_profile(ws_optarg, ws_optarg, true, &pf_filename,
                            &pf_dir_path, &pf_dir_path2) == -1) {
                        cmdarg_err("Can't copy file \"%s\" in directory\n\"%s\" to\n\"%s\":\n%s.",
                            pf_filename, pf_dir_path2, pf_dir_path, g_strerror(errno));

                        g_free(pf_filename);
                        g_free(pf_dir_path);
                        g_free(pf_dir_path2);
                        exit_status = WS_EXIT_INVALID_FILE;
                        goto clean_exit;
                    }
                    set_profile_name (ws_optarg);
                } else {
                    cmdarg_err("Configuration Profile \"%s\" does not exist", ws_optarg);
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case 'G':
                if (glossary != NULL) {
                    /* Multiple glossaries are difficult especially due to defaultprefs */
                    cmdarg_err("Multiple glossary reports (-G) are unsupported");
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                } else {
                    glossary = ws_optarg;
                }
                if (g_str_has_suffix(ws_optarg, "prefs")) {
                    has_extcap_options = true;
                }
                is_capturing = false;
                break;
            case 'i':
                has_extcap_options = true;
                break;
            case 'o':
                if (g_str_has_prefix(ws_optarg, "extcap.")) {
                    has_extcap_options = true;
                }
                break;
            case 'P':        /* Print packet summary info even when writing to a file */
                print_packet_info = true;
                print_summary = true;
                break;
            case 'r':        /* Read capture file x */
                cf_name = g_strdup(ws_optarg);
                is_capturing = false;
                break;
            case 'O':        /* Only output these protocols */
                output_only = g_strdup(ws_optarg);
                /* FALLTHROUGH */
            case 'V':        /* Verbose */
                print_details = true;
                print_packet_info = true;
                break;
            case 'x':        /* Print packet data in hex (and ASCII) */
                print_hex = true;
                /*  The user asked for hex output, so let's ensure they get it,
                 *  even if they're writing to a file.
                 */
                print_packet_info = true;
                break;
            case 'X':
                ex_opt_add(ws_optarg);
                break;
            case 'h':
            case 'v':
                is_capturing = false;
                break;
            default:
                break;
        }
    }

#ifndef HAVE_LUA
    if (ex_opt_count("lua_script") > 0) {
        cmdarg_err("This version of strato was not built with support for Lua scripting.");
        exit_status = WS_EXIT_INIT_FAILED;
        goto clean_exit;
    }
#endif /* HAVE_LUA */

    init_report_failure_message("strato");

    timestamp_set_type(TS_RELATIVE);
    timestamp_set_precision(TS_PREC_AUTO);
    timestamp_set_seconds_type(TS_SECONDS_DEFAULT);

    /*
     * Libwiretap must be initialized before libwireshark is, so that
     * dissection-time handlers for file-type-dependent blocks can
     * register using the file type/subtype value for the file type.
     */
    wtap_init(true);

    /* Register all dissectors; we must do this before checking for the
       "-G" flag, as the "-G" flag dumps information registered by the
       dissectors, and we must do it before we read the preferences, in
       case any dissectors register preferences. */
    if (!epan_init(NULL, NULL, true)) {
        exit_status = WS_EXIT_INIT_FAILED;
        goto clean_exit;
    }

    /* Register all tap listeners; we do this before we parse the arguments,
       as the "-z" argument can specify a registered tap. */

    register_all_tap_listeners(tap_reg_listener);

    /* Register extcap preferences only when needed. */
    if (has_extcap_options || is_capturing) {
        /*
         * XXX - We don't properly handle the capture_no_extcap preference.
         * To make it work, before registering the extcap preferences we'd
         * have to read at least that preference for the chosen profile, and
         * also check to make sure an "-o" option didn't override it.
         * Then, after registering the extcap preferences, we'd have to
         * set the extcap preferences from the preferences file and "-o"
         * options on the command line.
         */
        extcap_register_preferences();
    }

    conversation_table_set_gui_info(init_iousers);
    endpoint_table_set_gui_info(init_endpoints);
    srt_table_iterate_tables(register_srt_tables, NULL);
    rtd_table_iterate_tables(register_rtd_tables, NULL);
    stat_tap_iterate_tables(register_simple_stat_tables, NULL);

    ws_debug("strato reading settings");

    /* Load libwireshark settings from the current profile. */
    prefs_p = epan_load_settings();
    prefs_loaded = true;

    cap_file_init(&cfile);

    /* Print format defaults to this. */
    print_format = PR_FMT_TEXT;
    delimiter_char = " ";

    output_fields = output_fields_new();

    /*
     * To reset the options parser, set ws_optreset to 1 and set ws_optind to 1.
     *
     * Also reset ws_opterr to 1, so that error messages are printed by
     * getopt_long().
     */
    ws_optreset = 1;
    ws_optind = 1;
    ws_opterr = 1;

    /* Now get our args */
    while ((opt = ws_getopt_long(argc, argv, optstring, long_options, NULL)) != -1) {
        switch (opt) {
            case '2':        /* Perform two-pass analysis */
                if(epan_auto_reset){
                    cmdarg_err("-2 does not support auto session reset.");
                    arg_error=true;
                }
                perform_two_pass_analysis = true;
                break;
            case 'M':
                if(perform_two_pass_analysis){
                    cmdarg_err("-M does not support two-pass analysis.");
                    arg_error=true;
                }
                if (!get_positive_int(ws_optarg, "epan reset count", &epan_auto_reset_count))
                    arg_error = true;
                epan_auto_reset = true;
                break;
            case 'a':        /* autostop criteria */
            case 'b':        /* Ringbuffer option */
            case 'f':        /* capture filter */
            case 'g':        /* enable group read access on file(s) */
            case 'i':        /* Use interface x */
            case LONGOPT_SET_TSTAMP_TYPE: /* Set capture timestamp type */
            case 'p':        /* Don't capture in promiscuous mode */
#ifdef HAVE_PCAP_REMOTE
            case 'A':        /* Authentication */
#endif
            case 'I':        /* Capture in monitor mode, if available */
            case 's':        /* Set the snapshot (capture) length */
            case 'y':        /* Set the pcap data link type */
            case 'B':        /* Buffer size */
            case LONGOPT_NO_OPTIMIZE:          /* Don't optimize capture filter */
            case LONGOPT_COMPRESS_TYPE:        /* compress type */
            case LONGOPT_CAPTURE_TMPDIR:       /* capture temp directory */
            case LONGOPT_UPDATE_INTERVAL:      /* sync pipe update interval */
                /* These are options only for packet capture. */
                capture_option_specified = true;
                arg_error = true;
                break;
            case 'c':        /* Stop after x packets */
                if (!get_positive_int(ws_optarg, "packet count", &max_packet_count)) {
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case 'w':        /* Write to file x */
                output_file_name = g_strdup(ws_optarg);
                break;
            case 'C':
                /* already processed; just ignore it now */
                break;
            case 'D':        /* Print a list of capture devices and exit */
                capture_option_specified = true;
                arg_error = true;
                break;
            case 'e':
                /* Field entry */
                {
                    const char* col_field = try_convert_to_column_field(ws_optarg);
                    if (col_field) {
                        output_fields_add(output_fields, col_field);
                    } else {
                        header_field_info *hfi = proto_registrar_get_byalias(ws_optarg);
                        if (hfi)
                            output_fields_add(output_fields, hfi->abbrev);
                        else
                            output_fields_add(output_fields, ws_optarg);
                    }
                }
                break;
            case 'E':
                /* Field option */
                if (!output_fields_set_option(output_fields, ws_optarg)) {
                    cmdarg_err("\"%s\" is not a valid field output option=value pair.", ws_optarg);
                    output_fields_list_options(stderr);
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case 'F':
                out_file_type = wtap_name_to_file_type_subtype(ws_optarg);
                if (out_file_type < 0) {
                    cmdarg_err("\"%s\" isn't a valid capture file type", ws_optarg);
                    list_capture_types();
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case 'G':
                /* already processed; just ignore it now */
                break;
            case 'j':
                if (!protocolfilter_add_opt(ws_optarg, PF_NONE)) {
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case 'J':
                if (!protocolfilter_add_opt(ws_optarg, PF_INCLUDE_CHILDREN)) {
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case 'W':        /* Select extra information to save in our capture file */
                /* This is patterned after the -N flag which may not be the best idea. */
                if (strchr(ws_optarg, 'n')) {
                    out_file_name_res = true;
                } else {
                    cmdarg_err("Invalid -W argument \"%s\"; it must be one of:", ws_optarg);
                    cmdarg_err_cont("\t'n' write network address resolution information (pcapng only)");
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case 'H':        /* Read address to name mappings from a hosts file */
                if (! add_hosts_file(ws_optarg))
                {
                    cmdarg_err("Can't read host entries from \"%s\"", ws_optarg);
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                out_file_name_res = true;
                break;

            case 'h':        /* Print help and exit */
                show_help_header("Dump and analyze network traffic.");
                print_usage(stdout);
                exit_status = EXIT_SUCCESS;
                goto clean_exit;
                break;
            case 'l':        /* "Line-buffer" standard output */
                /* The ANSI C standard does not appear to *require* that a line-buffered
                   stream be flushed to the host environment whenever a newline is
                   written, it just says that, on such a stream, characters "are
                   intended to be transmitted to or from the host environment as a
                   block when a new-line character is encountered".

                   The Visual C++ 6.0 C implementation doesn't do what is intended;
                   even if you set a stream to be line-buffered, it still doesn't
                   flush the buffer at the end of every line.

                   The whole reason for the "-l" flag in either tcpdump, Tshark, or Strato
                   is to allow the output of a live capture to be piped to a program
                   or script and to have that script see the information for the
                   packet as soon as it's printed, rather than having to wait until
                   a standard I/O buffer fills up.

                   So, if the "-l" flag is specified, we flush the standard output
                   at the end of a packet.  This will do the right thing if we're
                   printing packet summary lines, and, as we print the entire protocol
                   tree for a single packet without waiting for anything to happen,
                   it should be as good as line-buffered mode if we're printing
                   protocol trees - arguably even better, as it may do fewer
                   writes. */
                line_buffered = true;
                break;
            case 'L':        /* Print list of link-layer types and exit */
                capture_option_specified = true;
                arg_error = true;
                break;
            case LONGOPT_LIST_TSTAMP_TYPES: /* List possible timestamp types */
                capture_option_specified = true;
                arg_error = true;
                break;
            case 'o':        /* Override preference from command line */
                {
                    char *errmsg = NULL;

                    switch (prefs_set_pref(ws_optarg, &errmsg)) {

                        case PREFS_SET_OK:
                            break;

                        case PREFS_SET_SYNTAX_ERR:
                            cmdarg_err("Invalid -o flag \"%s\"%s%s", ws_optarg,
                                    errmsg ? ": " : "", errmsg ? errmsg : "");
                            g_free(errmsg);
                            exit_status = WS_EXIT_INVALID_OPTION;
                            goto clean_exit;
                            break;

                        case PREFS_SET_NO_SUCH_PREF:
                            cmdarg_err("-o flag \"%s\" specifies unknown preference", ws_optarg);
                            exit_status = WS_EXIT_INVALID_OPTION;
                            goto clean_exit;
                            break;

                        case PREFS_SET_OBSOLETE:
                            cmdarg_err("-o flag \"%s\" specifies obsolete preference", ws_optarg);
                            exit_status = WS_EXIT_INVALID_OPTION;
                            goto clean_exit;
                            break;
                    }
                    break;
                }
            case 'q':        /* Quiet */
                quiet = true;
                break;
            case 'Q':        /* Really quiet */
                quiet = true;
                really_quiet = true;
                break;
            case 'r':
                /* already processed; just ignore it now */
                break;
            case 'R':        /* Read file filter */
                rfilter = ws_optarg;
                break;
            case 'P':
                /* already processed; just ignore it now */
                break;
            case 'S':        /* Set the line Separator to be printed between packets */
                separator = ws_optarg;
                break;
            case 'T':        /* printing Type */
                /* output_action has been already set. It means multiple -T. */
                if (output_action > WRITE_NONE) {
                    cmdarg_err("Multiple -T parameters are unsupported");
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                print_packet_info = true;
                if (strcmp(ws_optarg, "text") == 0) {
                    output_action = WRITE_TEXT;
                    print_format = PR_FMT_TEXT;
                } else if (strcmp(ws_optarg, "tabs") == 0) {
                    output_action = WRITE_TEXT;
                    print_format = PR_FMT_TEXT;
                    delimiter_char = "\t";
                } else if (strcmp(ws_optarg, "ps") == 0) {
                    output_action = WRITE_TEXT;
                    print_format = PR_FMT_PS;
                } else if (strcmp(ws_optarg, "pdml") == 0) {
                    output_action = WRITE_XML;
                    print_details = true;   /* Need details */
                    print_summary = false;  /* Don't allow summary */
                } else if (strcmp(ws_optarg, "psml") == 0) {
                    output_action = WRITE_XML;
                    print_details = false;  /* Don't allow details */
                    print_summary = true;   /* Need summary */
                } else if (strcmp(ws_optarg, "fields") == 0) {
                    output_action = WRITE_FIELDS;
                    print_details = true;   /* Need full tree info */
                    print_summary = false;  /* Don't allow summary */
                } else if (strcmp(ws_optarg, "json") == 0) {
                    output_action = WRITE_JSON;
                    print_details = true;   /* Need details */
                    print_summary = false;  /* Don't allow summary */
                } else if (strcmp(ws_optarg, "ek") == 0) {
                    output_action = WRITE_EK;
                    if (!print_summary)
                        print_details = true;
                } else if (strcmp(ws_optarg, "jsonraw") == 0) {
                    output_action = WRITE_JSON_RAW;
                    print_details = true;   /* Need details */
                    print_summary = false;  /* Don't allow summary */
                }
                else {
                    cmdarg_err("Invalid -T parameter \"%s\"; it must be one of:", ws_optarg);                   /* x */
                    cmdarg_err_cont("\t\"fields\"  The values of fields specified with the -e option, in a form\n"
                            "\t          specified by the -E option.\n"
                            "\t\"pdml\"    Packet Details Markup Language, an XML-based format for the\n"
                            "\t          details of a decoded packet. This information is equivalent to\n"
                            "\t          the packet details printed with the -V flag.\n"
                            "\t\"ps\"      PostScript for a human-readable one-line summary of each of\n"
                            "\t          the packets, or a multi-line view of the details of each of\n"
                            "\t          the packets, depending on whether the -V flag was specified.\n"
                            "\t\"psml\"    Packet Summary Markup Language, an XML-based format for the\n"
                            "\t          summary information of a decoded packet. This information is\n"
                            "\t          equivalent to the information shown in the one-line summary\n"
                            "\t          printed by default.\n"
                            "\t\"json\"    Packet Summary, an JSON-based format for the details\n"
                            "\t          summary information of a decoded packet. This information is \n"
                            "\t          equivalent to the packet details printed with the -V flag.\n"
                            "\t\"jsonraw\" Packet Details, a JSON-based format for machine parsing\n"
                            "\t          including only raw hex decoded fields (same as -T json -x but\n"
                            "\t          without text decoding, only raw fields included). \n"
                            "\t\"ek\"      Packet Details, an EK JSON-based format for the bulk insert \n"
                            "\t          into elastic search cluster. This information is \n"
                            "\t          equivalent to the packet details printed with the -V flag.\n"
                            "\t\"text\"    Text of a human-readable one-line summary of each of the\n"
                            "\t          packets, or a multi-line view of the details of each of the\n"
                            "\t          packets, depending on whether the -V flag was specified.\n"
                            "\t          This is the default.\n"
                            "\t\"tabs\"    Similar to the text report except that each column of the\n"
                            "\t          human-readable one-line summary is delimited with an ASCII\n"
                            "\t          horizontal tab character.");
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case 'U':        /* Export PDUs to file */
                if (strcmp(ws_optarg, "") == 0 || strcmp(ws_optarg, "?") == 0) {
                    list_export_pdu_taps();
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                pdu_export_arg = g_strdup(ws_optarg);
                break;
            case 'v':         /* Show version and exit */
#ifdef ENABLE_CHECK_FILTER
                /* If we're testing start-up of the epan library, go ahead and
                 * load all the delayed initialization prefixes (e.g. Diameter)
                 * so that they are reported on.
                 */
                proto_initialize_all_prefixes();
#endif
                show_version();
                /* We don't really have to cleanup here, but it's a convenient way to test
                 * start-up and shut-down of the epan library without any UI-specific
                 * cruft getting in the way. Makes the results of running
                 * $ ./tools/valgrind-wireshark -n
                 * much more useful. */
                epan_cleanup();
                extcap_cleanup();
                exit_status = EXIT_SUCCESS;
                goto clean_exit;
            case 'O':        /* Only output these protocols */
                /* already processed; just ignore it now */
                break;
            case 'V':        /* Verbose */
                /* already processed; just ignore it now */
                break;
            case 'x':        /* Print packet data in hex (and ASCII) */
                /* already processed; just ignore it now */
                break;
            case 'X':
                /* already processed; just ignore it now */
                break;
            case 'Y':
                dfilter = g_strdup(ws_optarg);
                break;
            case 'z':
                /* We won't call the init function for the stat this soon
                   as it would disallow MATE's fields (which are registered
                   by the preferences set callback) from being used as
                   part of a tap filter.  Instead, we just add the argument
                   to a list of stat arguments. */
                if (strcmp("help", ws_optarg) == 0) {
                    fprintf(stderr, "strato: The available statistics for the \"-z\" option are:\n");
                    list_stat_cmd_args();
                    exit_status = EXIT_SUCCESS;
                    goto clean_exit;
                }
                if (!process_stat_cmd_arg(ws_optarg)) {
                    cmdarg_err("Invalid -z argument \"%s\"; it must be one of:", ws_optarg);
                    list_stat_cmd_args();
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case 'd':        /* Decode as rule */
            case 'K':        /* Kerberos keytab file */
            case 'n':        /* No name resolution */
            case 'N':        /* Select what types of addresses/port #s to resolve */
            case 't':        /* Time stamp type */
            case 'u':        /* Seconds type */
            case LONGOPT_DISABLE_PROTOCOL: /* disable dissection of protocol */
            case LONGOPT_ENABLE_HEURISTIC: /* enable heuristic dissection of protocol */
            case LONGOPT_DISABLE_HEURISTIC: /* disable heuristic dissection of protocol */
            case LONGOPT_ENABLE_PROTOCOL: /* enable dissection of protocol (that is disabled by default) */
            case LONGOPT_ONLY_PROTOCOLS: /* enable dissection of only this comma separated list of protocols */
            case LONGOPT_DISABLE_ALL_PROTOCOLS: /* enable dissection of protocol (that is disabled by default) */
                if (!dissect_opts_handle_opt(opt, ws_optarg)) {
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case LONGOPT_EXPORT_OBJECTS:   /* --export-objects */
                if (strcmp("help", ws_optarg) == 0) {
                    fprintf(stderr, "strato: The available export object types for the \"--export-objects\" option are:\n");
                    eo_list_object_types();
                    exit_status = EXIT_SUCCESS;
                    goto clean_exit;
                }
                if (!eo_tap_opt_add(ws_optarg)) {
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case LONGOPT_EXPORT_TLS_SESSION_KEYS:   /* --export-tls-session-keys */
                tls_session_keys_file = ws_optarg;
                break;
            case LONGOPT_COLOR: /* print in color where appropriate */
                dissect_color = true;
                /* This has no effect if we don't print packet info or filter
                   (we can filter on the coloring rules). Should we warn or
                   error later if so, instead of silently ignoring it? */
                break;
            case LONGOPT_NO_DUPLICATE_KEYS:
                no_duplicate_keys = true;
                node_children_grouper = proto_node_group_children_by_json_key;
                break;
            case LONGOPT_ELASTIC_MAPPING_FILTER:
                /*
                 * XXX - A long option that exists to alter one other option
                 * (-G elastic-mapping) and for no other reason seems verbose.
                 * Deprecate in favor of -G elastic-mapping,<filter> ?
                 */
                elastic_mapping_filter = ws_optarg;
                break;
            case LONGOPT_CAPTURE_COMMENT:  /* capture comment */
                if (capture_comments == NULL) {
                    capture_comments = g_ptr_array_new_with_free_func(g_free);
                }
                g_ptr_array_add(capture_comments, g_strdup(ws_optarg));
                break;
            case LONGOPT_HEXDUMP:
                print_hex = true;
                print_packet_info = true;
                if (strcmp(ws_optarg, "all") == 0)
                    hexdump_source_option = HEXDUMP_SOURCE_MULTI;
                else if (strcmp(ws_optarg, "frames") == 0)
                    hexdump_source_option = HEXDUMP_SOURCE_PRIMARY;
                else if (strcmp(ws_optarg, "ascii") == 0)
                    hexdump_ascii_option = HEXDUMP_ASCII_INCLUDE;
                else if (strcmp(ws_optarg, "delimit") == 0)
                    hexdump_ascii_option = HEXDUMP_ASCII_DELIMIT;
                else if (strcmp(ws_optarg, "noascii") == 0)
                    hexdump_ascii_option = HEXDUMP_ASCII_EXCLUDE;
                else if (strcmp(ws_optarg, "time") == 0)
                    hexdump_timestamp_option = HEXDUMP_TIMESTAMP;
                else if (strcmp(ws_optarg, "notime") == 0)
                    hexdump_timestamp_option = HEXDUMP_TIMESTAMP_NONE;
                else if (strcmp("help", ws_optarg) == 0) {
                    hexdump_option_help(stdout);
                    exit_status = EXIT_SUCCESS;
                    goto clean_exit;
                } else {
                    fprintf(stderr, "strato: \"%s\" is an invalid value for --hexdump <hexoption>\n", ws_optarg);
                    fprintf(stderr, "For valid <hexoption> values enter: strato --hexdump help\n");
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case LONGOPT_SELECTED_FRAME:
                /* Hidden option to mark a frame as "selected". Used for testing and debugging.
                 * Only active in two-pass mode. */
                if (!ws_strtou32(ws_optarg, NULL, &selected_frame_number)) {
                    fprintf(stderr, "strato: \"%s\" is not a valid frame number\n", ws_optarg);
                    exit_status = WS_EXIT_INVALID_OPTION;
                    goto clean_exit;
                }
                break;
            case LONGOPT_PRINT_TIMERS:
                opt_print_timers = true;
                break;
            case LONGOPT_GLOBAL_PROFILE:
                /* already processed; just ignore it now */
                break;
            case LONGOPT_COMPRESS:        /* compress type */
                compression_type = ws_name_to_compression_type(ws_optarg);
                if (compression_type == WS_FILE_UNKNOWN_COMPRESSION) {
                    cmdarg_err("\"%s\" isn't a valid output compression mode",
                               ws_optarg);
                    list_output_compression_types();
                    goto clean_exit;
                }
                break;
            case '?':        /* Bad flag - print usage message */
            default:
                /* wslog arguments are okay */
                if (ws_log_is_wslog_arg(opt))
                    break;

                switch(ws_optopt) {
                    case 'F':
                        list_capture_types();
                        break;
                    case 'G':
                        glossary_option_help();
                        break;
                    case LONGOPT_COMPRESS:
                    case LONGOPT_COMPRESS_TYPE:
                        list_output_compression_types();
                        break;
                    default:
                        print_usage(stderr);
                }
                exit_status = WS_EXIT_INVALID_OPTION;
                goto clean_exit;
                break;
        }
    }

    /* set the default output action to TEXT */
    if (output_action == WRITE_NONE)
        output_action = WRITE_TEXT;

    /* set the default file type to pcapng */
    if (out_file_type == WTAP_FILE_TYPE_SUBTYPE_UNKNOWN)
        out_file_type = wtap_pcapng_file_type_subtype();

    /*
     * Print packet summary information is the default if neither -V or -x
     * were specified. Note that this is new behavior, which allows for the
     * possibility of printing only hex/ascii output without necessarily
     * requiring that either the summary or details be printed too.
     */
    if (!print_summary && !print_details && !print_hex)
        print_summary = true;

    if (no_duplicate_keys && output_action != WRITE_JSON && output_action != WRITE_JSON_RAW) {
        cmdarg_err("--no-duplicate-keys can only be used with \"-T json\" and \"-T jsonraw\"");
        exit_status = WS_EXIT_INVALID_OPTION;
        goto clean_exit;
    }

    /* If we specified output fields, but not the output field type... */
    /* XXX: If we specified both output fields with -e *and* protocol filters
     * with -j/-J, only the former are used. Should we warn or abort?
     * This also doesn't distinguish PDML from PSML, but shouldn't allow the
     * latter.
     */
    if ((WRITE_FIELDS != output_action && WRITE_XML != output_action && WRITE_JSON != output_action && WRITE_EK != output_action) && 0 != output_fields_num_fields(output_fields)) {
        cmdarg_err("Output fields were specified with \"-e\", "
                "but \"-Tek, -Tfields, -Tjson or -Tpdml\" was not specified.");
        exit_status = WS_EXIT_INVALID_OPTION;
        goto clean_exit;
    } else if (WRITE_FIELDS == output_action && 0 == output_fields_num_fields(output_fields)) {
        cmdarg_err("\"-Tfields\" was specified, but no fields were "
                "specified with \"-e\".");

        exit_status = WS_EXIT_INVALID_OPTION;
        goto clean_exit;
    }

    if (dissect_color) {
        if (!color_filters_init(&err_msg, NULL)) {
            fprintf(stderr, "%s\n", err_msg);
            g_free(err_msg);
        }
    }

    /* If no capture filter or display filter has been specified, and there are
       still command-line arguments, treat them as the tokens of a capture
       filter (if no "-r" flag was specified) or a display filter (if a "-r"
       flag was specified. */
    if (ws_optind < argc) {
        if (cf_name != NULL) {
            if (dfilter != NULL) {
                cmdarg_err("Display filters were specified both with \"-Y\" "
                        "and with additional command-line arguments.");
                exit_status = WS_EXIT_INVALID_OPTION;
                goto clean_exit;
            }
            dfilter = get_args_as_string(argc, argv, ws_optind);
        } else {
            capture_option_specified = true;
        }
    }

    if (!output_file_name) {
        /* We're not saving the capture to a file; if "-q" wasn't specified,
           we should print packet information */
        if (!quiet)
            print_packet_info = true;
    } else {
        const char *save_file = output_file_name;
        /* We're saving to a file; if we're writing to the standard output.
           and we'll also be writing dissected packets to the standard
           output, reject the request.  At best, we could redirect that
           to the standard error; we *can't* write both to the standard
           output and have either of them be useful. */
        if (strcmp(save_file, "-") == 0 && print_packet_info) {
            cmdarg_err("You can't write both raw packet data and dissected packets"
                    " to the standard output.");
            exit_status = WS_EXIT_INVALID_OPTION;
            goto clean_exit;
        }
        if (compression_type == WS_FILE_UNKNOWN_COMPRESSION) {
            /* An explicitly specified compression type overrides filename
             * magic. (Should we allow a way to specify "no" compression
             * with, e.g. a ".gz" extension?) */
            const char *sfx = strrchr(save_file, '.');
            if (sfx) {
                compression_type = ws_extension_to_compression_type(sfx + 1);
            }
        }
    }

    if (compression_type == WS_FILE_UNKNOWN_COMPRESSION) {
        compression_type = WS_FILE_UNCOMPRESSED;
    }

    if (!ws_can_write_compression_type(compression_type)) {
        cmdarg_err("Output files can't be written as %s",
                ws_compression_type_description(compression_type));
        exit_status = WS_EXIT_INVALID_OPTION;
        goto clean_exit;
    }

    if (compression_type != WS_FILE_UNCOMPRESSED && !wtap_dump_can_compress(out_file_type)) {
        cmdarg_err("The file format %s can't be written to output compressed format",
                wtap_file_type_subtype_name(out_file_type));
        exit_status = WS_EXIT_INVALID_OPTION;
        goto clean_exit;
    }

    /* XXX - We have two different long options for compression type;
     * one (undocumented) for live capturing and one for not. That is confusing.
     * LONGOPT_COMPRESS doesn't set "capture_option_specified" because it can be
     * used when capturing or when not capturing.
     */
    if (compression_type != WS_FILE_UNCOMPRESSED && is_capturing) {
        capture_option_specified = true;
        arg_error = true;
    }

    if (capture_option_specified)
        cmdarg_err("This version of strato was not built with support for capturing events.");
    if (arg_error) {
        print_usage(stderr);
        exit_status = WS_EXIT_INVALID_OPTION;
        goto clean_exit;
    }

    if (print_hex) {
        if (output_action != WRITE_TEXT && output_action != WRITE_JSON && output_action != WRITE_JSON_RAW && output_action != WRITE_EK) {
            cmdarg_err("Raw packet hex data can only be printed as text, PostScript, JSON, JSONRAW or EK JSON");
            exit_status = WS_EXIT_INVALID_OPTION;
            goto clean_exit;
        }
    }

    if (output_only != NULL) {
        char *ps;

        if (!print_details) {
            cmdarg_err("-O requires -V");
            exit_status = WS_EXIT_INVALID_OPTION;
            goto clean_exit;
        }

        output_only_tables = g_hash_table_new (g_str_hash, g_str_equal);
        for (ps = strtok (output_only, ","); ps; ps = strtok (NULL, ",")) {
            const char *name = ps;
            header_field_info *hfi = proto_registrar_get_byalias(name);
            if (hfi) {
                name = hfi->abbrev;
            }
            g_hash_table_insert(output_only_tables, (void *)name, (void *)name);
        }
    }

    if (rfilter != NULL && !perform_two_pass_analysis) {
        cmdarg_err("-R without -2 is deprecated. For single-pass filtering use -Y.");
        exit_status = WS_EXIT_INVALID_OPTION;
        goto clean_exit;
    }

    /*
     * If capture comments were specified, -w also has to have been specified.
     */
    if (capture_comments != NULL) {
        if (output_file_name) {
            /* They specified a "-w" flag, so we'll be saving to a capture file.
             * This is fine if they're writing in a format that supports
             * section block comments.
             */
            if (wtap_file_type_subtype_supports_option(out_file_type,
                        WTAP_BLOCK_SECTION,
                        OPT_COMMENT) == OPTION_NOT_SUPPORTED) {
                GArray *writable_type_subtypes;

                cmdarg_err("Capture comments can only be written to files of the following types:");
                writable_type_subtypes = wtap_get_writable_file_types_subtypes(FT_SORT_BY_NAME);
                for (unsigned i = 0; i < writable_type_subtypes->len; i++) {
                    int ft = g_array_index(writable_type_subtypes, int, i);

                    if (wtap_file_type_subtype_supports_option(ft, WTAP_BLOCK_SECTION,
                                OPT_COMMENT) != OPTION_NOT_SUPPORTED)
                        cmdarg_err_cont("    %s - %s", wtap_file_type_subtype_name(ft),
                                wtap_file_type_subtype_description(ft));
                }
                exit_status = WS_EXIT_INVALID_OPTION;
                goto clean_exit;
            }
        }
        else {
            cmdarg_err("Capture comments were specified, but you aren't writing a capture file.");
            exit_status = WS_EXIT_INVALID_OPTION;
            goto clean_exit;
        }
    }

    err_msg = ws_init_sockets();
    if (err_msg != NULL)
    {
        cmdarg_err("%s", err_msg);
        g_free(err_msg);
        cmdarg_err_cont("%s", please_report_bug());
        exit_status = WS_EXIT_INIT_FAILED;
        goto clean_exit;
    }

    /* Notify all registered modules that have had any of their preferences
       changed either from one of the preferences file or from the command
       line that their preferences have changed. */
    prefs_apply_all();

    /* We can also enable specified taps for export object */
    start_exportobjects();

    /* At this point MATE will have registered its field array so we can
       check if the fields specified by the user are all good.
       */
    {
        GSList* it = NULL;
        GSList *invalid_fields = output_fields_valid(output_fields);
        if (invalid_fields != NULL) {

            cmdarg_err("Some fields aren't valid:");
            for (it=invalid_fields; it != NULL; it = g_slist_next(it)) {
                cmdarg_err_cont("\t%s", (char *)it->data);
            }
            g_slist_free(invalid_fields);
            exit_status = WS_EXIT_INVALID_OPTION;
            goto clean_exit;
        }
    }

    if (ex_opt_count("read_format") > 0) {
        char* name = ex_opt_get_next("read_format");
        in_file_type = open_info_name_to_type(name);
        if (in_file_type == WTAP_TYPE_AUTO) {
            cmdarg_err("\"%s\" isn't a valid read file format type", name? name : "");
            g_free(name);
            list_read_capture_types();
            exit_status = WS_EXIT_INVALID_OPTION;
            goto clean_exit;
        }
        g_free(name);
    }

    if (global_dissect_options.time_format != TS_NOT_SET)
        timestamp_set_type(global_dissect_options.time_format);
    if (global_dissect_options.time_precision != TS_PREC_NOT_SET)
        timestamp_set_precision(global_dissect_options.time_precision);

    /*
     * Enabled and disabled protocols and heuristic dissectors as per
     * command-line options.
     */
    if (!setup_enabled_and_disabled_protocols()) {
        exit_status = WS_EXIT_INVALID_OPTION;
        goto clean_exit;
    }

    /* Build the column format array */
    build_column_format_array(&cfile.cinfo, prefs_p->num_cols, true);

    /* Everything is setup, dump glossaries now if that's what we're doing.
     * We want to do this after protocols and heuristic dissectors are
     * enabled and disabled. Doing it after building the column format
     * array might make it easier to add a report that describes the
     * current list of columns and how to add a new one (#17332).
     */
    if (glossary != NULL) {
        exit_status = dump_glossary(glossary, elastic_mapping_filter);
        goto clean_exit;
    }

    if (rfilter != NULL) {
        ws_debug("Compiling read filter: '%s'", rfilter);
        if (!compile_dfilter(rfilter, &rfcode)) {
            epan_cleanup();
            extcap_cleanup();

            exit_status = WS_EXIT_INVALID_INTERFACE;
            goto clean_exit;
        }
    }
    cfile.rfcode = rfcode;

    if (dfilter != NULL) {
        ws_debug("Compiling display filter: '%s'", dfilter);
        if (!compile_dfilter(dfilter, &dfcode)) {
            epan_cleanup();
            extcap_cleanup();

            exit_status = WS_EXIT_INVALID_FILTER;
            goto clean_exit;
        }
    }
    cfile.dfcode = dfcode;
    tap_load_main_filter(dfcode);

    if (print_packet_info) {
        /* If we're printing as text or PostScript, we have
           to create a print stream. */
        if (output_action == WRITE_TEXT) {
            switch (print_format) {

                case PR_FMT_TEXT:
                    print_stream = print_stream_text_stdio_new(stdout);
                    break;

                case PR_FMT_PS:
                    print_stream = print_stream_ps_stdio_new(stdout);
                    break;

                default:
                    ws_assert_not_reached();
            }
        }
    }

    /* PDU export requested. Take the ownership of the '-w' file, apply tap
     * filters and start tapping. */
    if (pdu_export_arg) {
        const char *exp_pdu_tap_name = pdu_export_arg;
        const char *exp_pdu_filter = dfilter; /* may be NULL to disable filter */
        char       *exp_pdu_error;
        int         exp_fd;
        char       *comment;

        if (!cf_name) {
            cmdarg_err("PDUs export requires a capture file (specify with -r).");
            exit_status = WS_EXIT_INVALID_OPTION;
            goto clean_exit;
        }
        /* Take ownership of the '-w' output file. */
        exp_pdu_filename = output_file_name;
        output_file_name = NULL;
        if (exp_pdu_filename == NULL) {
            cmdarg_err("PDUs export requires an output file (-w).");
            exit_status = WS_EXIT_INVALID_OPTION;
            goto clean_exit;
        }

        exp_pdu_error = exp_pdu_pre_open(exp_pdu_tap_name, exp_pdu_filter,
                &exp_pdu_tap_data);
        if (exp_pdu_error) {
            cmdarg_err("Cannot register tap: %s", exp_pdu_error);
            g_free(exp_pdu_error);
            list_export_pdu_taps();
            exit_status = INVALID_TAP;
            goto clean_exit;
        }

        if (strcmp(exp_pdu_filename, "-") == 0) {
            /* Write to the standard output. */
            exp_fd = 1;
        } else {
            exp_fd = ws_open(exp_pdu_filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
            if (exp_fd == -1) {
                cmdarg_err("%s: %s", exp_pdu_filename, file_open_error_message(errno, true));
                exit_status = WS_EXIT_INVALID_FILE;
                goto clean_exit;
            }
        }

        /* Activate the export PDU tap */
        /* Write to our output file with this comment (if the type supports it,
         * otherwise exp_pdu_open() will ignore the comment) */
        comment = ws_strdup_printf("Dump of PDUs from %s", cf_name);
        exp_pdu_status = exp_pdu_open(&exp_pdu_tap_data, exp_pdu_filename,
                out_file_type, exp_fd, comment,
                &err, &err_info);
        g_free(comment);
        if (!exp_pdu_status) {
            report_cfile_dump_open_failure(exp_pdu_filename, err, err_info,
                    out_file_type);
            exit_status = INVALID_EXPORT;
            goto clean_exit;
        }
    }

    if (cf_name) {
        ws_debug("strato: Opening capture file: %s", cf_name);
        /*
         * We're reading a capture file.
         */
        if (cf_open(&cfile, cf_name, in_file_type, false, &err) != CF_OK) {
            epan_cleanup();
            extcap_cleanup();
            exit_status = WS_EXIT_INVALID_FILE;
            goto clean_exit;
        }

        /* Start statistics taps; we do so after successfully opening the
           capture file, so we know we have something to compute stats
           on, and after registering all dissectors, so that MATE will
           have registered its field array so we can have a tap filter
           with one of MATE's late-registered fields as part of the
           filter. */
        if (!start_requested_stats()) {
            exit_status = WS_EXIT_INVALID_OPTION;
            goto clean_exit;
        }

        /* Do we need to do dissection of packets?  That depends on, among
           other things, what taps are listening, so determine that after
           starting the statistics taps. */
        do_dissection = must_do_dissection(rfcode, dfcode, pdu_export_arg);
        ws_debug("strato: do_dissection = %s", do_dissection ? "TRUE" : "FALSE");

        /* Process the packets in the file */
        ws_debug("strato: invoking process_cap_file() to process the packets");
        TRY {
            status = process_cap_file(&cfile, output_file_name, out_file_type, out_file_name_res,
            max_packet_count,
                0,
                0,
                WS_FILE_UNCOMPRESSED);
        }
        CATCH(OutOfMemoryError) {
            fprintf(stderr,
                    "Out Of Memory.\n"
                    "\n"
                    "Sorry, but strato has to terminate now.\n"
                    "\n"
                    "More information and workarounds can be found at\n"
                    WS_WIKI_URL("KnownBugs/OutOfMemory") "\n");
            status = PROCESS_FILE_ERROR;
        }
        ENDTRY;

        switch (status) {

            case PROCESS_FILE_SUCCEEDED:
                /* Everything worked OK; draw the taps. */
                draw_taps = true;
                break;

            case PROCESS_FILE_NO_FILE_PROCESSED:
                /* We never got to try to read the file, so there are no tap
                   results to dump.  Exit with an error status. */
                exit_status = 2;
                break;

            case PROCESS_FILE_ERROR:
                /* We still dump out the results of taps, etc., as we might have
                   read some packets; however, we exit with an error status. */
                draw_taps = true;
                exit_status = 2;
                break;

            case PROCESS_FILE_INTERRUPTED:
                /* The user interrupted the read process; Don't dump out the
                   result of taps, etc., and exit with an error status. */
                exit_status = 2;
                break;
        }

        if (pdu_export_arg) {
            if (!exp_pdu_close(&exp_pdu_tap_data, &err, &err_info)) {
                report_cfile_close_failure(exp_pdu_filename, err, err_info);
                exit_status = 2;
            }
            g_free(pdu_export_arg);
            g_free(exp_pdu_filename);
        }
    } else {
        ws_debug("strato: no capture file specified");
        /* No capture file specified, so we're supposed to do a live capture
           or get a list of link-layer types for a live capture device;
           do we have support for live captures? */
        /* No - complain. */
        cmdarg_err("No capture file or event source specified. Exiting.");
        exit_status = INVALID_CAPTURE;
        goto clean_exit;
    }

    if (cfile.provider.frames != NULL) {
        free_frame_data_sequence(cfile.provider.frames);
        cfile.provider.frames = NULL;
    }

    if (draw_taps)
        draw_tap_listeners(true);

    if (tls_session_keys_file) {
        size_t keylist_length = 0;
        unsigned num_keys = 0;
        char* keylist = NULL;
        secrets_export_values ret = secrets_export("TLS", &keylist, &keylist_length, &num_keys);
        if ((ret == SECRETS_EXPORT_SUCCESS) && (keylist_length > 0))
            write_file_binary_mode(tls_session_keys_file, keylist, keylist_length);
        g_free(keylist);
    }

    if (opt_print_timers) {
        if (cf_name == NULL) {
            /* We're doind a live capture. That isn't currently supported
             * with timers. */
            ws_message("Ignoring option --print-timers because we are doing a live capture");
        }
        else {
            print_elapsed_json(cf_name, dfilter);
        }
    }

    /* Memory cleanup */
    reset_tap_listeners();
    funnel_dump_all_text_windows();
    epan_free(cfile.epan);
    epan_cleanup();
    extcap_cleanup();

    output_fields_free(output_fields);
    output_fields = NULL;

clean_exit:
    cf_close(&cfile);
    g_free(cf_name);
    destroy_print_stream(print_stream);
    g_free(output_file_name);
    col_cleanup(&cfile.cinfo);
    wtap_cleanup();
    free_progdirs();
    dfilter_free(dfcode);
    g_free(dfilter);
    return exit_status;
}

bool loop_running;
uint32_t packet_count;

static epan_t *
strato_epan_new(capture_file *cf)
{
    static const struct packet_provider_funcs funcs = {
        cap_file_provider_get_frame_ts,
        cap_file_provider_get_start_ts,
        cap_file_provider_get_end_ts,
        cap_file_provider_get_interface_name,
        cap_file_provider_get_interface_description,
        NULL,
        NULL,
        NULL,
        NULL,
    };

    return epan_new(&cf->provider, &funcs);
}

static bool
process_packet_first_pass(capture_file *cf, epan_dissect_t *edt,
        int64_t offset, wtap_rec *rec)
{
    frame_data     fdlocal;
    uint32_t       framenum;
    bool           passed;
    int64_t        elapsed_start;

    /* The frame number of this packet is one more than the count of
       frames in this packet. */
    framenum = cf->count + 1;

    /* If we're not running a display filter and we're not printing any
       packet information, we don't need to do a dissection. This means
       that all packets can be marked as 'passed'. */
    passed = true;

    frame_data_init(&fdlocal, framenum, rec, offset, cum_bytes);

    /* If we're going to run a read filter or a display filter, set up to
       do a dissection and do so.  (This is the first pass of two passes
       over the packets, so we will not be printing any information
       from the dissection or running taps on the packet; if we're doing
       any of that, we'll do it in the second pass.) */
    if (edt) {
        if (gbl_resolv_flags.network_name || gbl_resolv_flags.maxmind_geoip) {
            /* If we're doing async lookups, send any that are queued and
             * retrieve results.
             *
             * Ideally we'd force any lookups that need to happen on the second pass
             * to be sent asynchronously on this pass so the results would be ready.
             * That will happen if they're involved in a filter (because we prime the
             * tree below), but not currently for taps, if we're printing packet
             * summaries or details, etc.
             *
             * XXX - If we're running a read filter that depends on a resolved
             * name, we should be doing synchronous lookups in that case. Also
             * marking the dependent frames below might not work with a display
             * filter that depends on a resolved name.
             */
            host_name_lookup_process();
        }

        column_info *cinfo = NULL;

        /* If we're running a read filter, prime the epan_dissect_t with that
           filter. */
        if (cf->rfcode)
            epan_dissect_prime_with_dfilter(edt, cf->rfcode);

        if (cf->dfcode)
            epan_dissect_prime_with_dfilter(edt, cf->dfcode);

        frame_data_set_before_dissect(&fdlocal, &cf->elapsed_time,
                &cf->provider.ref, cf->provider.prev_dis);
        if (cf->provider.ref == &fdlocal) {
            ref_frame = fdlocal;
            cf->provider.ref = &ref_frame;
        }

        /* If we're applying a filter that needs the columns, construct them. */
        if (dfilter_requires_columns(cf->rfcode) || dfilter_requires_columns(cf->dfcode)) {
            cinfo = &cf->cinfo;
        }

        elapsed_start = g_get_monotonic_time();
        epan_dissect_run(edt, cf->cd_t, rec, &fdlocal, cinfo);
        strato_elapsed.first_pass.dissect += g_get_monotonic_time() - elapsed_start;

        /* Run the read filter if we have one. */
        if (cf->rfcode) {
            elapsed_start = g_get_monotonic_time();
            passed = dfilter_apply_edt(cf->rfcode, edt);
            strato_elapsed.first_pass.dfilter_read += g_get_monotonic_time() - elapsed_start;
        }
    }

    if (passed) {
        frame_data_set_after_dissect(&fdlocal, &cum_bytes);
        cf->provider.prev_cap = cf->provider.prev_dis = frame_data_sequence_add(cf->provider.frames, &fdlocal);

        /* If we're not doing dissection then there won't be any dependent frames.
         * More importantly, edt.pi.fd.dependent_frames won't be initialized because
         * epan hasn't been initialized.
         * if we *are* doing dissection, then mark the dependent frames, but only
         * if a display filter was given and it matches this packet.
         */
        if (edt && cf->dfcode) {
            elapsed_start = g_get_monotonic_time();
            if (dfilter_apply_edt(cf->dfcode, edt) && edt->pi.fd->dependent_frames) {
                g_hash_table_foreach(edt->pi.fd->dependent_frames, find_and_mark_frame_depended_upon, cf->provider.frames);
            }

            if (selected_frame_number != 0 && selected_frame_number == cf->count + 1) {
                /* If we are doing dissection and we have a "selected frame"
                 * then load that frame's references (if any) onto the compiled
                 * display filter. Selected frame number is ordinal, count is cardinal. */
                dfilter_load_field_references(cf->dfcode, edt->tree);
            }
            strato_elapsed.first_pass.dfilter_filter += g_get_monotonic_time() - elapsed_start;
        }

        cf->count++;
    } else {
        /* if we don't add it to the frame_data_sequence, clean it up right now
         * to avoid leaks */
        frame_data_destroy(&fdlocal);
    }

    if (edt)
        epan_dissect_reset(edt);

    return passed;
}

/*
 * Set if reading a file was interrupted by a CTRL_ event on Windows or
 * a signal on UN*X.
 */
static bool read_interrupted;

#ifdef _WIN32
static BOOL WINAPI
read_cleanup(DWORD ctrltype _U_)
{
    /* CTRL_C_EVENT is sort of like SIGINT, CTRL_BREAK_EVENT is unique to
       Windows, CTRL_CLOSE_EVENT is sort of like SIGHUP, CTRL_LOGOFF_EVENT
       is also sort of like SIGHUP, and CTRL_SHUTDOWN_EVENT is sort of
       like SIGTERM at least when the machine's shutting down.

       For now, we handle them all as indications that we should clean up
       and quit, just as we handle SIGINT, SIGHUP, and SIGTERM in that
       way on UNIX.

       We must return true so that no other handler - such as one that would
       terminate the process - gets called.

       XXX - for some reason, typing ^C to Strato, if you run this in
       a Cygwin console window in at least some versions of Cygwin,
       causes strato to terminate immediately; this routine gets
       called, but the main loop doesn't get a chance to run and
       exit cleanly, at least if this is compiled with Microsoft Visual
       C++ (i.e., it's a property of the Cygwin console window or Bash;
       it happens if strato is not built with Cygwin - for all I know,
       building it with Cygwin may make the problem go away). */

    /* tell the read to stop */
    read_interrupted = true;

    return true;
}
#else
static void
read_cleanup(int signum _U_)
{
    /* tell the read to stop */
    read_interrupted = true;
}
#endif /* _WIN32 */

typedef enum {
    PASS_SUCCEEDED,
    PASS_READ_ERROR,
    PASS_WRITE_ERROR,
    PASS_INTERRUPTED
} pass_status_t;

static pass_status_t
process_cap_file_first_pass(capture_file *cf, int max_packet_count,
        int64_t max_byte_count, int *err, char **err_info)
{
    wtap_rec        rec;
    epan_dissect_t *edt = NULL;
    int64_t         data_offset;
    pass_status_t   status = PASS_SUCCEEDED;
    int             framenum = 0;

    wtap_rec_init(&rec, 1514);

    /* Allocate a frame_data_sequence for all the frames. */
    cf->provider.frames = new_frame_data_sequence();

    if (do_dissection) {
        bool create_proto_tree;

        /*
         * Determine whether we need to create a protocol tree.
         * We do if:
         *
         *    we're going to apply a read filter;
         *
         *    we're going to apply a display filter;
         *
         *    a postdissector wants field values or protocols
         *    on the first pass.
         */
        create_proto_tree =
            (cf->rfcode != NULL || cf->dfcode != NULL || postdissectors_want_hfids() || dissect_color);

        ws_debug("strato: create_proto_tree = %s", create_proto_tree ? "TRUE" : "FALSE");

        /* We're not going to display the protocol tree on this pass,
           so it's not going to be "visible". */
        edt = epan_dissect_new(cf->epan, create_proto_tree, false);
    }

    ws_debug("strato: reading records for first pass");
    *err = 0;
    while (wtap_read(cf->provider.wth, &rec, err, err_info, &data_offset)) {
        if (read_interrupted) {
            status = PASS_INTERRUPTED;
            break;
        }
        framenum++;

        if (process_packet_first_pass(cf, edt, data_offset, &rec)) {
            /* Stop reading if we hit a stop condition */
            if (max_packet_count > 0 && framenum >= max_packet_count) {
                ws_debug("strato: max_packet_count (%d) reached", max_packet_count);
                *err = 0; /* This is not an error */
                break;
            }
            if (max_byte_count != 0 && data_offset >= max_byte_count) {
                ws_debug("strato: max_byte_count (%" PRId64 "/%" PRId64 ") reached",
                        data_offset, max_byte_count);
                *err = 0; /* This is not an error */
                break;
            }
        }
        wtap_rec_reset(&rec);
    }
    if (*err != 0)
        status = PASS_READ_ERROR;

    if (edt)
        epan_dissect_free(edt);

    /* Close the sequential I/O side, to free up memory it requires. */
    wtap_sequential_close(cf->provider.wth);

    /* Allow the protocol dissectors to free up memory that they
     * don't need after the sequential run-through of the packets. */
    postseq_cleanup_all_protocols();

    cf->provider.prev_dis = NULL;
    cf->provider.prev_cap = NULL;

    wtap_rec_cleanup(&rec);

    return status;
}

static bool
process_packet_second_pass(capture_file *cf, epan_dissect_t *edt,
        frame_data *fdata, wtap_rec *rec, unsigned tap_flags _U_)
{
    column_info    *cinfo;
    bool            passed;
    wtap_block_t    block = NULL;
    int64_t         elapsed_start;

    /* If we're not running a display filter and we're not printing any
       packet information, we don't need to do a dissection. This means
       that all packets can be marked as 'passed'. */
    passed = true;

    /* If we're going to print packet information, or we're going to
       run a read filter, or we're going to process taps, set up to
       do a dissection and do so.  (This is the second pass of two
       passes over the packets; that's the pass where we print
       packet information or run taps.) */
    if (edt) {
        /* If we're running a display filter, prime the epan_dissect_t with that
           filter. */
        if (cf->dfcode)
            epan_dissect_prime_with_dfilter(edt, cf->dfcode);

        col_custom_prime_edt(edt, &cf->cinfo);

        output_fields_prime_edt(edt, output_fields);
        /* The PDML spec requires a 'geninfo' pseudo-protocol that needs
         * information from our 'frame' protocol.
         */
        if (output_fields_num_fields(output_fields) != 0 &&
                output_action == WRITE_XML) {
            epan_dissect_prime_with_hfid(edt, proto_registrar_get_id_byname("frame"));
        }

        /* We only need the columns if either
           1) some tap or filter needs the columns
           or
           2) we're printing packet info but we're *not* verbose; in verbose
           mode, we print the protocol tree, not the protocol summary.
           or
           3) there is a column mapped to an individual field
           */
        if ((tap_listeners_require_columns()) || (print_packet_info && print_summary) || output_fields_has_cols(output_fields) || dfilter_requires_columns(cf->dfcode))
            cinfo = &cf->cinfo;
        else
            cinfo = NULL;

        frame_data_set_before_dissect(fdata, &cf->elapsed_time,
                &cf->provider.ref, cf->provider.prev_dis);
        if (cf->provider.ref == fdata) {
            ref_frame = *fdata;
            cf->provider.ref = &ref_frame;
        }

        if (dissect_color) {
            color_filters_prime_edt(edt);
            fdata->need_colorize = 1;
        }

        /* Initialize passed_dfilter here so that dissectors can hide packets. */
        /* XXX We might want to add a separate "visible" bit to frame_data instead. */
        fdata->passed_dfilter = 1;

        /* epan_dissect_run (and epan_dissect_reset) unref the block.
         * We need it later, e.g. in order to copy the options. */
        block = wtap_block_ref(rec->block);
        elapsed_start = g_get_monotonic_time();
        epan_dissect_run_with_taps(edt, cf->cd_t, rec, fdata, cinfo);
        strato_elapsed.second_pass.dissect += g_get_monotonic_time() - elapsed_start;
        passed = fdata->passed_dfilter == 1;

        /* Run the display filter if we have one. */
        if (fdata->passed_dfilter && cf->dfcode) {
            elapsed_start = g_get_monotonic_time();
            passed = dfilter_apply_edt(cf->dfcode, edt);
            strato_elapsed.second_pass.dfilter_filter += g_get_monotonic_time() - elapsed_start;
        }
    }

    if (passed) {
        frame_data_set_after_dissect(fdata, &cum_bytes);
        /* Process this packet. */
        if (print_packet_info) {
            /* We're printing packet information; print the information for
               this packet. */
            print_packet(cf, edt);

            /* If we're doing "line-buffering", flush the standard output
               after every packet.  See the comment above, for the "-l"
               option, for an explanation of why we do that. */
            if (line_buffered)
                fflush(stdout);

            if (ferror(stdout)) {
                show_print_file_io_error();
                return false;
            }
        }
        cf->provider.prev_dis = fdata;
    }
    cf->provider.prev_cap = fdata;

    if (edt) {
        epan_dissect_reset(edt);
        rec->block = block;
    }
    return passed || fdata->dependent_of_displayed;
}

static bool
process_new_idbs(wtap *wth, wtap_dumper *pdh, int *err, char **err_info)
{
    wtap_block_t if_data;

    while ((if_data = wtap_get_next_interface_description(wth)) != NULL) {
        /*
         * Only add interface blocks if the output file supports (meaning
         * *requires*) them.
         *
         * That mean that the abstract interface provided by libwiretap
         * involves WTAP_BLOCK_IF_ID_AND_INFO blocks.
         */
        if (pdh != NULL) {
            if (wtap_file_type_subtype_supports_block(wtap_dump_file_type_subtype(pdh), WTAP_BLOCK_IF_ID_AND_INFO) != BLOCK_NOT_SUPPORTED) {
                if (!wtap_dump_add_idb(pdh, if_data, err, err_info))
                    return false;
            }
        }
    }
    return true;
}

static pass_status_t
process_cap_file_second_pass(capture_file *cf, wtap_dumper *pdh,
        int *err, char **err_info,
        volatile uint32_t *err_framenum,
        int max_write_packet_count)
{
    wtap_rec        rec;
    int             framenum = 0;
    int             write_framenum = 0;
    frame_data     *fdata;
    bool            filtering_tap_listeners;
    unsigned        tap_flags;
    epan_dissect_t *edt = NULL;
    pass_status_t   status = PASS_SUCCEEDED;

    /*
     * Process whatever IDBs we haven't seen yet.  This will be all
     * the IDBs in the file, as we've finished reading it; they'll
     * all be at the beginning of the output file.
     */
    if (!process_new_idbs(cf->provider.wth, pdh, err, err_info)) {
        *err_framenum = 0;
        return PASS_WRITE_ERROR;
    }

    wtap_rec_init(&rec, 1514);

    /* Do we have any tap listeners with filters? */
    filtering_tap_listeners = have_filtering_tap_listeners();

    /* Get the union of the flags for all tap listeners. */
    tap_flags = union_of_tap_listener_flags();

    if (do_dissection) {
        bool create_proto_tree;

        /*
         * Determine whether we need to create a protocol tree.
         * We do if:
         *
         *    we're going to apply a display filter;
         *
         *    we're going to print the protocol tree;
         *
         *    one of the tap listeners requires a protocol tree;
         *
         *    we have custom columns (which require field values, which
         *    currently requires that we build a protocol tree).
         */
        create_proto_tree =
            (cf->dfcode || print_details || filtering_tap_listeners ||
             (tap_flags & TL_REQUIRES_PROTO_TREE) || have_custom_cols(&cf->cinfo) || dissect_color);

        ws_debug("strato: create_proto_tree = %s", create_proto_tree ? "TRUE" : "FALSE");

        /* The protocol tree will be "visible", i.e., nothing faked, only if
           we're printing packet details, which is true if we're printing stuff
           ("print_packet_info" is true) and we're in verbose mode
           ("packet_details" is true). But if we specified certain fields with
           "-e", we'll prime those directly later. */
        bool visible = print_packet_info && print_details && output_fields_num_fields(output_fields) == 0;
        edt = epan_dissect_new(cf->epan, create_proto_tree, visible);
    }

    /*
     * Force synchronous resolution of IP addresses; in this pass, we
     * can't do it in the background and fix up past dissections.
     */
    set_resolution_synchrony(true);

    for (framenum = 1; framenum <= (int)cf->count; framenum++) {
        if (read_interrupted) {
            status = PASS_INTERRUPTED;
            break;
        }
        fdata = frame_data_sequence_find(cf->provider.frames, framenum);
        if (!wtap_seek_read(cf->provider.wth, fdata->file_off, &rec, err,
                    err_info)) {
            /* Error reading from the input file. */
            status = PASS_READ_ERROR;
            break;
        }
        ws_debug("strato: invoking process_packet_second_pass() for frame #%d", framenum);
        if (process_packet_second_pass(cf, edt, fdata, &rec, tap_flags)) {
            /* Either there's no read filtering or this packet passed the
               filter, so, if we're writing to a capture file, write
               this packet out. */
            write_framenum++;
            if (pdh != NULL) {
                ws_debug("strato: writing packet #%d to outfile packet #%d", framenum, write_framenum);
                if (!wtap_dump(pdh, &rec, err, err_info)) {
                    /* Error writing to the output file. */
                    ws_debug("strato: error writing to a capture file (%d)", *err);
                    *err_framenum = framenum;
                    status = PASS_WRITE_ERROR;
                    break;
                }
                /* Stop reading if we hit a stop condition */
                if (max_write_packet_count > 0 && write_framenum >= max_write_packet_count) {
                    ws_debug("strato: max_write_packet_count (%d) reached", max_write_packet_count);
                    *err = 0; /* This is not an error */
                    break;
                }
            }
        }
        wtap_rec_reset(&rec);
    }

    if (edt)
        epan_dissect_free(edt);

    wtap_rec_cleanup(&rec);

    return status;
}

static pass_status_t
process_cap_file_single_pass(capture_file *cf, wtap_dumper *pdh,
        int max_packet_count, int64_t max_byte_count,
        int max_write_packet_count,
        int *err, char **err_info,
        volatile uint32_t *err_framenum)
{
    wtap_rec        rec;
    bool create_proto_tree = false;
    bool            filtering_tap_listeners;
    unsigned        tap_flags;
    int             framenum = 0;
    int             write_framenum = 0;
    epan_dissect_t *edt = NULL;
    int64_t         data_offset;
    pass_status_t   status = PASS_SUCCEEDED;
    bool            visible = false;

    wtap_rec_init(&rec, 1514);

    /* Do we have any tap listeners with filters? */
    filtering_tap_listeners = have_filtering_tap_listeners();

    /* Get the union of the flags for all tap listeners. */
    tap_flags = union_of_tap_listener_flags();

    if (do_dissection) {
        /*
         * Determine whether we need to create a protocol tree.
         * We do if:
         *
         *    we're going to apply a read filter;
         *
         *    we're going to apply a display filter;
         *
         *    we're going to print the protocol tree;
         *
         *    one of the tap listeners is going to apply a filter;
         *
         *    one of the tap listeners requires a protocol tree;
         *
         *    a postdissector wants field values or protocols
         *    on the first pass;
         *
         *    we have custom columns (which require field values, which
         *    currently requires that we build a protocol tree).
         */
        create_proto_tree =
            (cf->rfcode || cf->dfcode || print_details || filtering_tap_listeners ||
             (tap_flags & TL_REQUIRES_PROTO_TREE) || postdissectors_want_hfids() ||
             have_custom_cols(&cf->cinfo) || dissect_color);

        ws_debug("strato: create_proto_tree = %s", create_proto_tree ? "TRUE" : "FALSE");

        /* The protocol tree will be "visible", i.e., nothing faked, only if
           we're printing packet details, which is true if we're printing stuff
           ("print_packet_info" is true) and we're in verbose mode
           ("packet_details" is true). But if we specified certain fields with
           "-e", we'll prime those directly later. */
        visible = print_packet_info && print_details && output_fields_num_fields(output_fields) == 0;
        edt = epan_dissect_new(cf->epan, create_proto_tree, visible);
    }

    /*
     * Force synchronous resolution of IP addresses; we're doing only
     * one pass, so we can't do it in the background and fix up past
     * dissections.
     */
    set_resolution_synchrony(true);

    *err = 0;
    while (wtap_read(cf->provider.wth, &rec, err, err_info, &data_offset)) {
        if (read_interrupted) {
            status = PASS_INTERRUPTED;
            break;
        }
        framenum++;

        /*
         * Process whatever IDBs we haven't seen yet.
         */
        if (!process_new_idbs(cf->provider.wth, pdh, err, err_info)) {
            *err_framenum = framenum;
            status = PASS_WRITE_ERROR;
            break;
        }

        ws_debug("strato: processing packet #%d", framenum);

        reset_epan_mem(cf, edt, create_proto_tree, visible);

        if (process_packet_single_pass(cf, edt, data_offset, &rec, tap_flags)) {
            /* Either there's no read filtering or this packet passed the
               filter, so, if we're writing to a capture file, write
               this packet out. */
            write_framenum++;
            if (pdh != NULL) {
                ws_debug("strato: writing packet #%d to outfile as #%d",
                        framenum, write_framenum);
                if (!wtap_dump(pdh, &rec, err, err_info)) {
                    /* Error writing to the output file. */
                    ws_debug("strato: error writing to a capture file (%d)", *err);
                    *err_framenum = framenum;
                    status = PASS_WRITE_ERROR;
                    break;
                }
            }
        }
        /* Stop reading if we hit a stop condition */
        if (max_packet_count > 0 && framenum >= max_packet_count) {
            ws_debug("strato: max_packet_count (%d) reached", max_packet_count);
            *err = 0; /* This is not an error */
            break;
        }
        if (max_write_packet_count > 0 && write_framenum >= max_write_packet_count) {
            ws_debug("strato: max_write_packet_count (%d) reached", max_write_packet_count);
            *err = 0; /* This is not an error */
            break;
        }
        if (max_byte_count != 0 && data_offset >= max_byte_count) {
            ws_debug("strato: max_byte_count (%" PRId64 "/%" PRId64 ") reached",
                    data_offset, max_byte_count);
            *err = 0; /* This is not an error */
            break;
        }
        wtap_rec_reset(&rec);
    }
    if (status == PASS_SUCCEEDED) {
        if (*err != 0) {
            /* Error reading from the input file. */
            status = PASS_READ_ERROR;
        } else {
            /*
             * Process whatever IDBs we haven't seen yet.
             */
            if (!process_new_idbs(cf->provider.wth, pdh, err, err_info)) {
                *err_framenum = framenum;
                status = PASS_WRITE_ERROR;
            }
        }
    }

    if (edt)
        epan_dissect_free(edt);

    wtap_rec_cleanup(&rec);

    return status;
}

static process_file_status_t
process_cap_file(capture_file *cf, char *save_file, int out_file_type,
        bool out_file_name_res, int max_packet_count, int64_t max_byte_count,
        int max_write_packet_count, ws_compression_type compression_type)
{
    process_file_status_t status = PROCESS_FILE_SUCCEEDED;
    wtap_dumper *pdh;
#ifndef _WIN32
    struct sigaction  action, oldaction;
#endif
    int          err = 0, err_pass1 = 0;
    char        *err_info = NULL, *err_info_pass1 = NULL;
    volatile uint32_t err_framenum;
    wtap_dump_params params = WTAP_DUMP_PARAMS_INIT;
    char        *shb_user_appl;
    pass_status_t first_pass_status, second_pass_status;
    int64_t elapsed_start;

    if (save_file != NULL) {
        /* Set up to write to the capture file. */
        wtap_dump_params_init_no_idbs(&params, cf->provider.wth);

        /* If we don't have an application name add strato */
        if (wtap_block_get_string_option_value(g_array_index(params.shb_hdrs, wtap_block_t, 0), OPT_SHB_USERAPPL, &shb_user_appl) != WTAP_OPTTYPE_SUCCESS) {
            /* this is free'd by wtap_block_unref() later */
            wtap_block_add_string_option_format(g_array_index(params.shb_hdrs, wtap_block_t, 0), OPT_SHB_USERAPPL, "%s", get_appname_and_version());
        }
        if (capture_comments != NULL) {
            for (unsigned i = 0; i < capture_comments->len; i++) {
                wtap_block_add_string_option_format(g_array_index(params.shb_hdrs, wtap_block_t, 0),
                        OPT_COMMENT, "%s",
                        (char *)g_ptr_array_index(capture_comments, i));
            }
        }

        ws_debug("strato: writing format type %d, to %s", out_file_type, save_file);
        if (strcmp(save_file, "-") == 0) {
            /* Write to the standard output. */
            pdh = wtap_dump_open_stdout(out_file_type, compression_type, &params,
                    &err, &err_info);
        } else {
            pdh = wtap_dump_open(save_file, out_file_type, compression_type, &params,
                    &err, &err_info);
        }

        g_free(params.idb_inf);
        params.idb_inf = NULL;

        if (pdh == NULL) {
            /* We couldn't set up to write to the capture file. */
            report_cfile_dump_open_failure(save_file, err, err_info,
                    out_file_type);
            status = PROCESS_FILE_NO_FILE_PROCESSED;
            goto out;
        }
    } else {
        /* Set up to print packet information. */
        if (print_packet_info) {
            if (!write_preamble(cf)) {
                show_print_file_io_error();
                status = PROCESS_FILE_NO_FILE_PROCESSED;
                goto out;
            }
        }
        pdh = NULL;
    }

#ifdef _WIN32
    /* Catch a CTRL+C event and, if we get it, clean up and exit. */
    SetConsoleCtrlHandler(read_cleanup, true);
#else /* _WIN32 */
    /* Catch SIGINT and SIGTERM and, if we get either of them,
       clean up and exit.  If SIGHUP isn't being ignored, catch
       it too and, if we get it, clean up and exit.

       We restart any read that was in progress, so that it doesn't
       disrupt reading from the sync pipe.  The signal handler tells
       the capture child to finish; it will report that it finished,
       or will exit abnormally, so  we'll stop reading from the sync
       pipe, pick up the exit status, and quit. */
    memset(&action, 0, sizeof(action));
    action.sa_handler = read_cleanup;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, NULL, &oldaction);
    if (oldaction.sa_handler == SIG_DFL)
        sigaction(SIGHUP, &action, NULL);
#endif /* _WIN32 */

    if (perform_two_pass_analysis) {
        ws_debug("strato: perform_two_pass_analysis, do_dissection=%s", do_dissection ? "TRUE" : "FALSE");

        elapsed_start = g_get_monotonic_time();
        first_pass_status = process_cap_file_first_pass(cf, max_packet_count,
                max_byte_count,
                &err_pass1,
                &err_info_pass1);
        strato_elapsed.elapsed_first_pass = g_get_monotonic_time() - elapsed_start;

        ws_debug("strato: done with first pass");

        if (first_pass_status == PASS_INTERRUPTED) {
            /* The first pass was interrupted; skip the second pass.
               It won't be run, so it won't get an error. */
            second_pass_status = PASS_SUCCEEDED;
        } else {
            /*
             * If we got a read error on the first pass, we still do the second
             * pass, so we can at least process the packets we read, and then
             * report the first-pass error after the second pass (and before
             * we report any second-pass errors), so all the errors show up
             * at the end.
             */
            elapsed_start = g_get_monotonic_time();
            second_pass_status = process_cap_file_second_pass(cf, pdh, &err, &err_info,
                    &err_framenum,
                    max_write_packet_count);
            strato_elapsed.elapsed_second_pass = g_get_monotonic_time() - elapsed_start;

            ws_debug("strato: done with second pass");
        }
    }
    else {
        /* !perform_two_pass_analysis */
        ws_debug("strato: perform one pass analysis, do_dissection=%s", do_dissection ? "TRUE" : "FALSE");

        first_pass_status = PASS_SUCCEEDED; /* There is no first pass */

        elapsed_start = g_get_monotonic_time();
        second_pass_status = process_cap_file_single_pass(cf, pdh,
                max_packet_count,
                max_byte_count,
                max_write_packet_count,
                &err, &err_info,
                &err_framenum);
        strato_elapsed.elapsed_first_pass = g_get_monotonic_time() - elapsed_start;

        ws_debug("strato: done with single pass");
    }

    if (first_pass_status != PASS_SUCCEEDED ||
            second_pass_status != PASS_SUCCEEDED) {
        /*
         * At least one of the passes didn't succeed; either it got a failure
         * or it was interrupted.
         */
        if (first_pass_status != PASS_INTERRUPTED ||
                second_pass_status != PASS_INTERRUPTED) {
            /* At least one of the passes got an error. */
            ws_debug("strato: something failed along the line (%d)", err);
            /*
             * If we're printing packet data, and the standard output and error
             * are going to the same place, flush the standard output, so everything
             * buffered up is written, and then print a newline to the standard
             * error before printing the error message, to separate it from the
             * packet data.  (Alas, that only works on UN*X; st_dev is meaningless,
             * and the _fstat() documentation at Microsoft doesn't indicate whether
             * st_ino is even supported.)
             */
#ifndef _WIN32
            if (print_packet_info) {
                ws_statb64 stat_stdout, stat_stderr;

                if (ws_fstat64(1, &stat_stdout) == 0 && ws_fstat64(2, &stat_stderr) == 0) {
                    if (stat_stdout.st_dev == stat_stderr.st_dev &&
                            stat_stdout.st_ino == stat_stderr.st_ino) {
                        fflush(stdout);
                        fprintf(stderr, "\n");
                    }
                }
            }
#endif
        }
        /* Report status of pass 1 of two-pass processing. */
        switch (first_pass_status) {

            case PASS_SUCCEEDED:
                /* No problem. */
                break;

            case PASS_READ_ERROR:
                /* Read error. */
                report_cfile_read_failure(cf->filename, err_pass1, err_info_pass1);
                status = PROCESS_FILE_ERROR;
                break;

            case PASS_WRITE_ERROR:
                /* Won't happen on the first pass. */
                break;

            case PASS_INTERRUPTED:
                /* Not an error, so nothing to report. */
                status = PROCESS_FILE_INTERRUPTED;
                break;
        }

        /* Report status of pass 2 of two-pass processing or the only pass
           of one-pass processing. */
        switch (second_pass_status) {

            case PASS_SUCCEEDED:
                /* No problem. */
                break;

            case PASS_READ_ERROR:
                /* Read error. */
                report_cfile_read_failure(cf->filename, err, err_info);
                status = PROCESS_FILE_ERROR;
                break;

            case PASS_WRITE_ERROR:
                /* Write error.
                   XXX - framenum is not necessarily the frame number in
                   the input file if there was a read filter. */
                report_cfile_write_failure(cf->filename, save_file, err, err_info,
                        err_framenum, out_file_type);
                status = PROCESS_FILE_ERROR;
                break;

            case PASS_INTERRUPTED:
                /* Not an error, so nothing to report. */
                status = PROCESS_FILE_INTERRUPTED;
                break;
        }
    }
    if (save_file != NULL) {
        if (second_pass_status != PASS_WRITE_ERROR) {
            if (pdh && out_file_name_res) {
                /* XXX: This doesn't work as expected. First, it should be
                 * moved to between the first and second passes (if doing
                 * two-pass mode), so that the new NRB appears before packets,
                 * which is better for subsequent one-pass mode. It never works
                 * well in one-pass mode.
                 *
                 * Second, it only writes hosts that we've done lookups for,
                 * which means unless packet details are printed (or there's
                 * a display filter that matches something that will do a host
                 * lookup, e.g. -Y "ip") it doesn't actually have anything
                 * in the list to save. Notably, that includes the case of
                 * "strato [-2] -H hosts.txt -r <infile> -w <outfile>",
                 * which a user would certainly expect to dissect packets,
                 * lookup hostnames, and add them to an NRB for later use.
                 * A workaround is if "-V > /dev/null" is added, but who
                 * expects that?
                 *
                 * A third issue is that name resolution blocks aren't
                 * written for live captures.
                 */
                if (!wtap_dump_set_addrinfo_list(pdh, get_addrinfo_list())) {
                    cmdarg_err("The file format \"%s\" doesn't support name resolution information.",
                            wtap_file_type_subtype_name(out_file_type));
                }
            }
            /* Now close the capture file. */
            if (!wtap_dump_close(pdh, NULL, &err, &err_info)) {
                report_cfile_close_failure(save_file, err, err_info);
                status = PROCESS_FILE_ERROR;
            }
        } else {
            /* We got a write error; it was reported, so just close the dump file
               without bothering to check for further errors. */
            wtap_dump_close(pdh, NULL, &err, &err_info);
            g_free(err_info);
            status = PROCESS_FILE_ERROR;
        }
    } else {
        if (print_packet_info) {
            if (!write_finale()) {
                show_print_file_io_error();
                status = PROCESS_FILE_ERROR;
            }
        }
    }

out:
    wtap_close(cf->provider.wth);
    cf->provider.wth = NULL;

    wtap_dump_params_cleanup(&params);

    return status;
}

static bool
process_packet_single_pass(capture_file *cf, epan_dissect_t *edt, int64_t offset,
        wtap_rec *rec, unsigned tap_flags _U_)
{
    frame_data      fdata;
    column_info    *cinfo;
    bool            passed;
    wtap_block_t    block = NULL;
    int64_t         elapsed_start;

    /* Count this packet. */
    cf->count++;

    /* If we're not running a display filter and we're not printing any
       packet information, we don't need to do a dissection. This means
       that all packets can be marked as 'passed'. */
    passed = true;

    frame_data_init(&fdata, cf->count, rec, offset, cum_bytes);

    /* If we're going to print packet information, or we're going to
       run a read filter, or we're going to process taps, set up to
       do a dissection and do so.  (This is the one and only pass
       over the packets, so, if we'll be printing packet information
       or running taps, we'll be doing it here.) */
    if (edt) {
        /* If we're running a filter, prime the epan_dissect_t with that
           filter. */
        if (cf->dfcode)
            epan_dissect_prime_with_dfilter(edt, cf->dfcode);

        col_custom_prime_edt(edt, &cf->cinfo);

        output_fields_prime_edt(edt, output_fields);
        /* The PDML spec requires a 'geninfo' pseudo-protocol that needs
         * information from our 'frame' protocol.
         */
        if (output_fields_num_fields(output_fields) != 0 &&
                output_action == WRITE_XML) {
            epan_dissect_prime_with_hfid(edt, proto_registrar_get_id_byname("frame"));
        }

        /* We only need the columns if either
           1) some tap or filter needs the columns
           or
           2) we're printing packet info but we're *not* verbose; in verbose
           mode, we print the protocol tree, not the protocol summary.
           or
           3) there is a column mapped as an individual field */
        if ((tap_listeners_require_columns()) || (print_packet_info && print_summary) || output_fields_has_cols(output_fields) || dfilter_requires_columns(cf->dfcode))
            cinfo = &cf->cinfo;
        else
            cinfo = NULL;

        frame_data_set_before_dissect(&fdata, &cf->elapsed_time,
                &cf->provider.ref, cf->provider.prev_dis);
        if (cf->provider.ref == &fdata) {
            ref_frame = fdata;
            cf->provider.ref = &ref_frame;
        }

        if (dissect_color) {
            color_filters_prime_edt(edt);
            fdata.need_colorize = 1;
        }

        /* Initialize passed_dfilter here so that dissectors can hide packets. */
        /* XXX We might want to add a separate "visible" bit to frame_data instead. */
        fdata.passed_dfilter = 1;

        /* epan_dissect_run (and epan_dissect_reset) unref the block.
         * We need it later, e.g. in order to copy the options. */
        block = wtap_block_ref(rec->block);
        elapsed_start = g_get_monotonic_time();
        epan_dissect_run_with_taps(edt, cf->cd_t, rec, &fdata, cinfo);
        strato_elapsed.first_pass.dissect += g_get_monotonic_time() - elapsed_start;
        passed = fdata.passed_dfilter == 1;

        /* Run the filter if we have it. */
        if (fdata.passed_dfilter && cf->dfcode) {
            elapsed_start = g_get_monotonic_time();
            passed = dfilter_apply_edt(cf->dfcode, edt);
            strato_elapsed.first_pass.dfilter_filter += g_get_monotonic_time() - elapsed_start;
        }
    }

    if (passed) {
        frame_data_set_after_dissect(&fdata, &cum_bytes);

        /* Process this packet. */
        if (print_packet_info) {
            /* We're printing packet information; print the information for
               this packet. */
            ws_assert(edt);
            print_packet(cf, edt);

            /* If we're doing "line-buffering", flush the standard output
               after every packet.  See the comment above, for the "-l"
               option, for an explanation of why we do that. */
            if (line_buffered)
                fflush(stdout);

            if (ferror(stdout)) {
                show_print_file_io_error();
                return false;
            }
        }

        /* this must be set after print_packet() [bug #8160] */
        prev_dis_frame = fdata;
        cf->provider.prev_dis = &prev_dis_frame;
    }

    prev_cap_frame = fdata;
    cf->provider.prev_cap = &prev_cap_frame;

    if (edt) {
        epan_dissect_reset(edt);
        frame_data_destroy(&fdata);
        rec->block = block;
    }
    return passed;
}

static bool
write_preamble(capture_file *cf)
{
    switch (output_action) {

        case WRITE_TEXT:
            return print_preamble(print_stream, cf->filename, get_ws_vcs_version_info());

        case WRITE_XML:
            if (print_details)
                write_pdml_preamble(stdout, cf->filename);
            else
                write_psml_preamble(&cf->cinfo, stdout);
            return !ferror(stdout);

        case WRITE_FIELDS:
            write_fields_preamble(output_fields, stdout);
            return !ferror(stdout);

        case WRITE_JSON:
        case WRITE_JSON_RAW:
            jdumper = write_json_preamble(stdout);
            return !ferror(stdout);

        case WRITE_EK:
            return true;

        default:
            ws_assert_not_reached();
            return false;
    }
}

static char *
get_line_buf(size_t len)
{
    static char   *line_bufp    = NULL;
    static size_t  line_buf_len = 256;
    size_t         new_line_buf_len;

    for (new_line_buf_len = line_buf_len; len > new_line_buf_len;
            new_line_buf_len *= 2)
        ;
    if (line_bufp == NULL) {
        line_buf_len = new_line_buf_len;
        line_bufp = (char *)g_malloc(line_buf_len + 1);
    } else {
        if (new_line_buf_len > line_buf_len) {
            line_buf_len = new_line_buf_len;
            line_bufp = (char *)g_realloc(line_bufp, line_buf_len + 1);
        }
    }
    return line_bufp;
}

static inline void
put_string(char *dest, const char *str, size_t str_len)
{
    memcpy(dest, str, str_len);
    dest[str_len] = '\0';
}

static inline void
put_spaces_string(char *dest, const char *str, size_t str_len, size_t str_with_spaces)
{
    size_t i;

    for (i = str_len; i < str_with_spaces; i++)
        *dest++ = ' ';

    put_string(dest, str, str_len);
}

static inline void
put_string_spaces(char *dest, const char *str, size_t str_len, size_t str_with_spaces)
{
    size_t i;

    memcpy(dest, str, str_len);
    for (i = str_len; i < str_with_spaces; i++)
        dest[i] = ' ';

    dest[str_with_spaces] = '\0';
}

static bool
print_columns(capture_file *cf, const epan_dissect_t *edt)
{
    char   *line_bufp;
    int     i;
    size_t  buf_offset;
    size_t  column_len;
    size_t  col_len;
    col_item_t* col_item;
    char str_format[11];
    const color_filter_t *color_filter = NULL;

    line_bufp = get_line_buf(256);
    buf_offset = 0;
    *line_bufp = '\0';

    if (dissect_color)
        color_filter = edt->pi.fd->color_filter;

    for (i = 0; i < cf->cinfo.num_cols; i++) {
        col_item = &cf->cinfo.columns[i];
        /* Skip columns not marked as visible. */
        if (!get_column_visible(i))
            continue;
        const char* col_text = get_column_text(&cf->cinfo, i);
        switch (col_item->col_fmt) {
            case COL_NUMBER:
            case COL_NUMBER_DIS:
                column_len = col_len = strlen(col_text);
                if (column_len < 5)
                    column_len = 5;
                line_bufp = get_line_buf(buf_offset + column_len);
                put_spaces_string(line_bufp + buf_offset, col_text, col_len, column_len);
                break;

            case COL_CLS_TIME:
            case COL_REL_TIME:
            case COL_ABS_TIME:
            case COL_ABS_YMD_TIME:  /* XXX - wider */
            case COL_ABS_YDOY_TIME: /* XXX - wider */
            case COL_UTC_TIME:
            case COL_UTC_YMD_TIME:  /* XXX - wider */
            case COL_UTC_YDOY_TIME: /* XXX - wider */
                column_len = col_len = strlen(col_text);
                if (column_len < 10)
                    column_len = 10;
                line_bufp = get_line_buf(buf_offset + column_len);
                put_spaces_string(line_bufp + buf_offset, col_text, col_len, column_len);
                break;

            case COL_DEF_SRC:
            case COL_RES_SRC:
            case COL_UNRES_SRC:
            case COL_DEF_DL_SRC:
            case COL_RES_DL_SRC:
            case COL_UNRES_DL_SRC:
            case COL_DEF_NET_SRC:
            case COL_RES_NET_SRC:
            case COL_UNRES_NET_SRC:
                column_len = col_len = strlen(col_text);
                if (column_len < 12)
                    column_len = 12;
                line_bufp = get_line_buf(buf_offset + column_len);
                put_spaces_string(line_bufp + buf_offset, col_text, col_len, column_len);
                break;

            case COL_DEF_DST:
            case COL_RES_DST:
            case COL_UNRES_DST:
            case COL_DEF_DL_DST:
            case COL_RES_DL_DST:
            case COL_UNRES_DL_DST:
            case COL_DEF_NET_DST:
            case COL_RES_NET_DST:
            case COL_UNRES_NET_DST:
                column_len = col_len = strlen(col_text);
                if (column_len < 12)
                    column_len = 12;
                line_bufp = get_line_buf(buf_offset + column_len);
                put_string_spaces(line_bufp + buf_offset, col_text, col_len, column_len);
                break;

            default:
                column_len = strlen(col_text);
                line_bufp = get_line_buf(buf_offset + column_len);
                put_string(line_bufp + buf_offset, col_text, column_len);
                break;
        }
        buf_offset += column_len;
        if (i != cf->cinfo.num_cols - 1) {
            /*
             * This isn't the last column, so we need to print a
             * separator between this column and the next.
             *
             * If we printed a network source and are printing a
             * network destination of the same type next, separate
             * them with a UTF-8 right arrow; if we printed a network
             * destination and are printing a network source of the same
             * type next, separate them with a UTF-8 left arrow;
             * otherwise separate them with a space.
             *
             * We add enough space to the buffer for " \xe2\x86\x90 "
             * or " \xe2\x86\x92 ", even if we're only adding " ".
             */
            line_bufp = get_line_buf(buf_offset + 5);
            switch (col_item->col_fmt) {

                case COL_DEF_SRC:
                case COL_RES_SRC:
                case COL_UNRES_SRC:
                    switch (cf->cinfo.columns[i+1].col_fmt) {

                        case COL_DEF_DST:
                        case COL_RES_DST:
                        case COL_UNRES_DST:
                            snprintf(str_format, sizeof(str_format), "%s%s%s", delimiter_char, UTF8_RIGHTWARDS_ARROW, delimiter_char);
                            put_string(line_bufp + buf_offset, str_format, 5);
                            buf_offset += 5;
                            break;

                        default:
                            put_string(line_bufp + buf_offset, delimiter_char, 1);
                            buf_offset += 1;
                            break;
                    }
                    break;

                case COL_DEF_DL_SRC:
                case COL_RES_DL_SRC:
                case COL_UNRES_DL_SRC:
                    switch (cf->cinfo.columns[i+1].col_fmt) {

                        case COL_DEF_DL_DST:
                        case COL_RES_DL_DST:
                        case COL_UNRES_DL_DST:
                            snprintf(str_format, sizeof(str_format), "%s%s%s", delimiter_char, UTF8_RIGHTWARDS_ARROW, delimiter_char);
                            put_string(line_bufp + buf_offset, str_format, 5);
                            buf_offset += 5;
                            break;

                        default:
                            put_string(line_bufp + buf_offset, delimiter_char, 1);
                            buf_offset += 1;
                            break;
                    }
                    break;

                case COL_DEF_NET_SRC:
                case COL_RES_NET_SRC:
                case COL_UNRES_NET_SRC:
                    switch (cf->cinfo.columns[i+1].col_fmt) {

                        case COL_DEF_NET_DST:
                        case COL_RES_NET_DST:
                        case COL_UNRES_NET_DST:
                            snprintf(str_format, sizeof(str_format), "%s%s%s", delimiter_char, UTF8_RIGHTWARDS_ARROW, delimiter_char);
                            put_string(line_bufp + buf_offset, str_format, 5);
                            buf_offset += 5;
                            break;

                        default:
                            put_string(line_bufp + buf_offset, delimiter_char, 1);
                            buf_offset += 1;
                            break;
                    }
                    break;

                case COL_DEF_DST:
                case COL_RES_DST:
                case COL_UNRES_DST:
                    switch (cf->cinfo.columns[i+1].col_fmt) {

                        case COL_DEF_SRC:
                        case COL_RES_SRC:
                        case COL_UNRES_SRC:
                            snprintf(str_format, sizeof(str_format), "%s%s%s", delimiter_char, UTF8_LEFTWARDS_ARROW, delimiter_char);
                            put_string(line_bufp + buf_offset, str_format, 5);
                            buf_offset += 5;
                            break;

                        default:
                            put_string(line_bufp + buf_offset, delimiter_char, 1);
                            buf_offset += 1;
                            break;
                    }
                    break;

                case COL_DEF_DL_DST:
                case COL_RES_DL_DST:
                case COL_UNRES_DL_DST:
                    switch (cf->cinfo.columns[i+1].col_fmt) {

                        case COL_DEF_DL_SRC:
                        case COL_RES_DL_SRC:
                        case COL_UNRES_DL_SRC:
                            snprintf(str_format, sizeof(str_format), "%s%s%s", delimiter_char, UTF8_LEFTWARDS_ARROW, delimiter_char);
                            put_string(line_bufp + buf_offset, str_format, 5);
                            buf_offset += 5;
                            break;

                        default:
                            put_string(line_bufp + buf_offset, delimiter_char, 1);
                            buf_offset += 1;
                            break;
                    }
                    break;

                case COL_DEF_NET_DST:
                case COL_RES_NET_DST:
                case COL_UNRES_NET_DST:
                    switch (cf->cinfo.columns[i+1].col_fmt) {

                        case COL_DEF_NET_SRC:
                        case COL_RES_NET_SRC:
                        case COL_UNRES_NET_SRC:
                            snprintf(str_format, sizeof(str_format), "%s%s%s", delimiter_char, UTF8_LEFTWARDS_ARROW, delimiter_char);
                            put_string(line_bufp + buf_offset, str_format, 5);
                            buf_offset += 5;
                            break;

                        default:
                            put_string(line_bufp + buf_offset, delimiter_char, 1);
                            buf_offset += 1;
                            break;
                    }
                    break;

                default:
                    put_string(line_bufp + buf_offset, delimiter_char, 1);
                    buf_offset += 1;
                    break;
            }
        }
    }

    if (dissect_color && color_filter != NULL)
        return print_line_color(print_stream, 0, line_bufp, &color_filter->fg_color, &color_filter->bg_color);
    else
        return print_line(print_stream, 0, line_bufp);
}

static bool
print_packet(capture_file *cf, epan_dissect_t *edt)
{
    if (print_summary || output_fields_has_cols(output_fields))
        /* Just fill in the columns. */
        epan_dissect_fill_in_columns(edt, false, true);

    /* Print summary columns and/or protocol tree */
    switch (output_action) {

        case WRITE_TEXT:
            if (print_summary && !print_columns(cf, edt))
                return false;
            if (print_details) {
                if (!proto_tree_print(print_details ? print_dissections_expanded : print_dissections_none,
                            print_hex, edt, output_only_tables, print_stream))
                    return false;
                if (!print_hex) {
                    if (!print_line(print_stream, 0, separator))
                        return false;
                }
            }
            break;

        case WRITE_XML:
            if (print_summary) {
                write_psml_columns(edt, stdout, dissect_color);
                return !ferror(stdout);
            }
            if (print_details) {
                write_pdml_proto_tree(output_fields, edt, &cf->cinfo, stdout, dissect_color);
                printf("\n");
                return !ferror(stdout);
            }
            break;

        case WRITE_FIELDS:
            if (print_summary) {
                /*No non-verbose "fields" format */
                ws_assert_not_reached();
            }
            if (print_details) {
                write_fields_proto_tree(output_fields, edt, &cf->cinfo, stdout);
                printf("\n");
                return !ferror(stdout);
            }
            break;

        case WRITE_JSON:
            if (print_summary)
                ws_assert_not_reached();
            if (print_details) {
                write_json_proto_tree(output_fields, print_dissections_expanded,
                        print_hex, edt, &cf->cinfo, node_children_grouper, &jdumper);
                return !ferror(stdout);
            }
            break;

        case WRITE_JSON_RAW:
            if (print_summary)
                ws_assert_not_reached();
            if (print_details) {
                write_json_proto_tree(output_fields, print_dissections_none,
                        true, edt, &cf->cinfo, node_children_grouper, &jdumper);
                return !ferror(stdout);
            }
            break;

        case WRITE_EK:
            write_ek_proto_tree(output_fields, print_summary, print_hex,
                    edt, &cf->cinfo, stdout);
            return !ferror(stdout);

        default:
            ws_assert_not_reached();
    }

    if (print_hex) {
        if (print_summary || print_details) {
            if (!print_line(print_stream, 0, ""))
                return false;
        }
        if (!print_hex_data(print_stream, edt, hexdump_source_option | hexdump_ascii_option | hexdump_timestamp_option))
            return false;
        if (!print_line(print_stream, 0, separator))
            return false;
    }
    return true;
}

static bool
write_finale(void)
{
    switch (output_action) {

        case WRITE_TEXT:
            return print_finale(print_stream);

        case WRITE_XML:
            if (print_details)
                write_pdml_finale(stdout);
            else
                write_psml_finale(stdout);
            return !ferror(stdout);

        case WRITE_FIELDS:
            write_fields_finale(output_fields, stdout);
            return !ferror(stdout);

        case WRITE_JSON:
        case WRITE_JSON_RAW:
            write_json_finale(&jdumper);
            return !ferror(stdout);

        case WRITE_EK:
            return true;

        default:
            ws_assert_not_reached();
            return false;
    }
}

void
cf_close(capture_file *cf)
{
    if (cf->state == FILE_CLOSED)
        return; /* Nothing to do */

    if (cf->provider.wth != NULL) {
        wtap_close(cf->provider.wth);
        cf->provider.wth = NULL;
    }
    /* We have no file open... */
    if (cf->filename != NULL) {
        /* If it's a temporary file, remove it. */
        if (cf->is_tempfile)
            ws_unlink(cf->filename);
        g_free(cf->filename);
        cf->filename = NULL;
    }

    /* We have no file open. */
    cf->state = FILE_CLOSED;
}

cf_status_t
cf_open(capture_file *cf, const char *fname, unsigned int type, bool is_tempfile, int *err)
{
    wtap  *wth;
    char *err_info;

    wth = wtap_open_offline(fname, type, err, &err_info, perform_two_pass_analysis);
    if (wth == NULL)
        goto fail;

    /* The open succeeded.  Fill in the information for this file. */

    cf->provider.wth = wth;
    cf->f_datalen = 0; /* not used, but set it anyway */

    /* Set the file name because we need it to set the follow stream filter.
       XXX - is that still true?  We need it for other reasons, though,
       in any case. */
    cf->filename = g_strdup(fname);

    /* Indicate whether it's a permanent or temporary file. */
    cf->is_tempfile = is_tempfile;

    /* No user changes yet. */
    cf->unsaved_changes = false;

    cf->cd_t      = wtap_file_type_subtype(cf->provider.wth);
    cf->open_type = type;
    cf->count     = 0;
    cf->drops_known = false;
    cf->drops     = 0;
    cf->snap      = wtap_snapshot_length(cf->provider.wth);
    nstime_set_zero(&cf->elapsed_time);
    cf->provider.ref = NULL;
    cf->provider.prev_dis = NULL;
    cf->provider.prev_cap = NULL;

    cf->state = FILE_READ_IN_PROGRESS;

    /* Create new epan session for dissection. */
    epan_free(cf->epan);
    cf->epan = strato_epan_new(cf);

    wtap_set_cb_new_ipv4(cf->provider.wth, add_ipv4_name);
    wtap_set_cb_new_ipv6(cf->provider.wth, (wtap_new_ipv6_callback_t) add_ipv6_name);
    wtap_set_cb_new_secrets(cf->provider.wth, secrets_wtap_callback);

    return CF_OK;

fail:
    report_cfile_open_failure(fname, *err, err_info);
    return CF_ERROR;
}

static void
show_print_file_io_error(void)
{
    switch (errno) {

        case ENOSPC:
            cmdarg_err("Not all the packets could be printed because there is "
                    "no space left on the file system.");
            break;

#ifdef EDQUOT
        case EDQUOT:
            cmdarg_err("Not all the packets could be printed because you are "
                    "too close to, or over your disk quota.");
            break;
#endif

        case EPIPE:
            /*
             * This almost certainly means "the next program after us in
             * the pipeline exited before we were finished writing", so
             * this isn't a real error, it just means we're done.  (We
             * don't get SIGPIPE because libwireshark ignores SIGPIPE
             * to avoid getting killed if writing to the MaxMind process
             * gets SIGPIPE because that process died.)
             *
             * Presumably either that program exited deliberately (for
             * example, "head -N" read N lines and printed them), in
             * which case there's no error to report, or it terminated
             * due to an error or a signal, in which case *that's* the
             * error and that error has been reported.
             */
            break;

        default:
#ifdef _WIN32
            if (errno == EINVAL && _doserrno == ERROR_NO_DATA) {
                /*
                 * XXX - on Windows, a write to a pipe where the read side
                 * has been closed apparently may return the Windows error
                 * ERROR_BROKEN_PIPE, which the Visual Studio C library maps
                 * to EPIPE, or may return the Windows error ERROR_NO_DATA,
                 * which the Visual Studio C library maps to EINVAL.
                 *
                 * Either of those almost certainly means "the next program
                 * after us in the pipeline exited before we were finished
                 * writing", so, if _doserrno is ERROR_NO_DATA, this isn't
                 * a real error, it just means we're done.  (Windows doesn't
                 * SIGPIPE.)
                 *
                 * Presumably either that program exited deliberately (for
                 * example, "head -N" read N lines and printed them), in
                 * which case there's no error to report, or it terminated
                 * due to an error or a signal, in which case *that's* the
                 * error and that error has been reported.
                 */
                break;
            }

            /*
             * It's a different error; report it, but with the error
             * message for _doserrno, which will give more detail
             * than just "Invalid argument".
             */
            cmdarg_err("An error occurred while printing packets: %s.",
                    win32strerror(_doserrno));
#else
            cmdarg_err("An error occurred while printing packets: %s.",
                    g_strerror(errno));
#endif
            break;
    }
}

static void
reset_epan_mem(capture_file *cf,epan_dissect_t *edt, bool tree, bool visual)
{
    if (!epan_auto_reset || (cf->count < epan_auto_reset_count) || !edt)
        return;

    fprintf(stderr, "resetting session.\n");

    epan_dissect_cleanup(edt);
    epan_free(cf->epan);

    cf->epan = strato_epan_new(cf);
    epan_dissect_init(edt, cf->epan, tree, visual);
    cf->count = 0;
}
