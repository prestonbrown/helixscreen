/**
 * @file lv_xml_dropdown_parser.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_xml_dropdown_parser.h"
#if LV_USE_XML && LV_USE_DROPDOWN

#include <lvgl.h>
#include <lvgl_private.h>
#include "../lv_xml_private.h"
#if LV_USE_TRANSLATION
    #include "../../others/translation/lv_translation.h"
#endif

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
#if LV_USE_TRANSLATION
static void dropdown_translate_options(lv_obj_t * dd, const char * tags);
static void dropdown_on_language_changed(lv_event_t * e);
static void dropdown_on_delete_free_tags(lv_event_t * e);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void * lv_xml_dropdown_create(lv_xml_parser_state_t * state, const char ** attrs)
{
    LV_UNUSED(attrs);
    void * item = lv_dropdown_create(lv_xml_state_get_parent(state));

    return item;
}


void lv_xml_dropdown_apply(lv_xml_parser_state_t * state, const char ** attrs)
{
    void * item = lv_xml_state_get_item(state);

    lv_xml_obj_apply(state, attrs); /*Apply the common properties, e.g. width, height, styles flags etc*/

    for(int i = 0; attrs[i]; i += 2) {
        const char * name = attrs[i];
        const char * value = attrs[i + 1];

        if(lv_streq("options", name)) lv_dropdown_set_options(item, value);
        else if(lv_streq("text", name)) lv_dropdown_set_text(item, value);
        else if(lv_streq("selected", name)) lv_dropdown_set_selected(item, lv_xml_atoi(value));
        else if(lv_streq("symbol", name)) lv_dropdown_set_symbol(item, lv_xml_get_image(&state->scope, value));
        else if(lv_streq("bind_value", name)) {
            lv_subject_t * subject = lv_xml_get_subject(&state->scope, value);
            if(subject) {
                lv_dropdown_bind_value(item, subject);
            }
            else {
                LV_LOG_WARN("Subject \"%s\" doesn't exist in dropdown bind_value", value);
            }
        }
#if LV_USE_TRANSLATION
        else if(lv_streq("options_tag", name)) {
            if(value[0] == '\0') continue;  /* Skip empty tags */
            char * tags_copy = lv_strdup(value);
            dropdown_translate_options(item, value);
            lv_obj_add_event_cb(item, dropdown_on_language_changed,
                                LV_EVENT_TRANSLATION_LANGUAGE_CHANGED, tags_copy);
            lv_obj_add_event_cb(item, dropdown_on_delete_free_tags,
                                LV_EVENT_DELETE, tags_copy);
        }
#endif
    }
}

void * lv_xml_dropdown_list_create(lv_xml_parser_state_t * state, const char ** attrs)
{
    LV_UNUSED(attrs);

    return lv_dropdown_get_list(lv_xml_state_get_parent(state));
}

void lv_xml_dropdown_list_apply(lv_xml_parser_state_t * state, const char ** attrs)
{
    LV_UNUSED(state);
    LV_UNUSED(attrs);

    lv_xml_obj_apply(state, attrs);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#if LV_USE_TRANSLATION
static void dropdown_translate_options(lv_obj_t * dd, const char * tags)
{
    /* Count total length needed */
    uint32_t total_len = 0;
    const char * p = tags;
    while(*p) {
        const char * nl = lv_strchr(p, '\n');
        uint32_t seg_len = nl ? (uint32_t)(nl - p) : lv_strlen(p);

        /* Temporarily null-terminate this segment for lv_tr() */
        char tmp[256];
        if(seg_len >= sizeof(tmp)) seg_len = sizeof(tmp) - 1;
        lv_memcpy(tmp, p, seg_len);
        tmp[seg_len] = '\0';

        const char * translated = lv_tr(tmp);
        total_len += lv_strlen(translated) + 1; /* +1 for \n or \0 */

        if(nl) p = nl + 1;
        else break;
    }

    if(total_len == 0) return;

    char * buf = lv_malloc(total_len);
    if(!buf) return;

    char * dst = buf;
    p = tags;
    while(*p) {
        const char * nl = lv_strchr(p, '\n');
        uint32_t seg_len = nl ? (uint32_t)(nl - p) : lv_strlen(p);

        char tmp[256];
        if(seg_len >= sizeof(tmp)) seg_len = sizeof(tmp) - 1;
        lv_memcpy(tmp, p, seg_len);
        tmp[seg_len] = '\0';

        const char * translated = lv_tr(tmp);
        uint32_t tlen = lv_strlen(translated);
        lv_memcpy(dst, translated, tlen);
        dst += tlen;

        if(nl) {
            *dst = '\n';
            dst++;
            p = nl + 1;
        }
        else {
            break;
        }
    }
    *dst = '\0';

    lv_dropdown_set_options(dd, buf);
    lv_free(buf);
}

static void dropdown_on_language_changed(lv_event_t * e)
{
    lv_obj_t * dd = lv_event_get_target(e);
    const char * tags = (const char *)lv_event_get_user_data(e);
    if(tags) dropdown_translate_options(dd, tags);
}

static void dropdown_on_delete_free_tags(lv_event_t * e)
{
    char * tags = (char *)lv_event_get_user_data(e);
    if(tags) lv_free(tags);
}
#endif /* LV_USE_TRANSLATION */

#endif /* LV_USE_XML */
