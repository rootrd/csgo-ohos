/* Adapter for the Parsifal SAX API used by Panorama layouts and SVGs.
 * Expat owns XML validation, encodings, entity handling and streaming.
 * Parsifal's input callback returns BIS_EOF (1) at EOF, including a final
 * nonempty chunk. XMLParser_Parse returns 1 on success and 0 on failure;
 * XML_OK / XML_ABORT are callback results, not parse results. */
#include <expat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "libparsifal/parsifal.h"

typedef struct {
    XMLPARSER public;
    XML_Parser expat;
    int callback_aborted;
} Parser;

typedef struct {
    char *storage;
    const XMLCH *uri, *local, *qname, *prefix;
} Name;

static int make_name(Parser *p, const char *expanded, Name *name)
{
    size_t len = strlen(expanded);
    char *first, *second, *qname;
    memset(name, 0, sizeof(*name));
    name->storage = (char *)malloc(len * 2 + 3);
    if (!name->storage) return 0;
    memcpy(name->storage, expanded, len + 1);
    name->uri = name->prefix = (const XMLCH *)"";
    name->local = name->qname = (const XMLCH *)name->storage;
    if (!(p->public.XMLFlags & XMLFLAG_NAMESPACES)) return 1;
    first = strchr(name->storage, '\x1f');
    if (!first) return 1;
    *first++ = 0;
    name->uri = (const XMLCH *)name->storage;
    name->local = name->qname = (const XMLCH *)first;
    second = strchr(first, '\x1f');
    if (second) {
        *second++ = 0;
        name->prefix = (const XMLCH *)second;
        qname = name->storage + len + 1;
        snprintf(qname, len + 2, "%s:%s", second, first);
        name->qname = (const XMLCH *)qname;
    }
    return 1;
}

static void abort_callback(Parser *p, int code)
{
    p->callback_aborted = code;
    XML_StopParser(p->expat, XML_FALSE);
}

static void XMLCALL start_element(void *userdata, const char *name, const char **attributes)
{
    Parser *p = (Parser *)userdata;
    XMLVECTOR vector = {0};
    Name element = {0}, *names = NULL;
    XMLRUNTIMEATT *items = NULL;
    int count = 0, i, built = 0;
    if (!p->public.startElementHandler) return;
    while (attributes[count * 2]) ++count;
    if (!make_name(p, name, &element)) goto memory_error;
    if (count) {
        items = (XMLRUNTIMEATT *)calloc((size_t)count, sizeof(*items));
        names = (Name *)calloc((size_t)count, sizeof(*names));
        if (!items || !names) goto memory_error;
    }
    for (i = 0; i < count; ++i) {
        if (!make_name(p, attributes[i * 2], &names[i])) goto memory_error;
        ++built;
        items[i].qname = (XMLCH *)names[i].qname;
        items[i].localName = (XMLCH *)names[i].local;
        items[i].prefix = (XMLCH *)names[i].prefix;
        items[i].uri = (XMLCH *)names[i].uri;
        items[i].value = (XMLCH *)attributes[i * 2 + 1];
        items[i].nameBuf.str = items[i].qname;
    }
    vector.array = (BYTE *)items;
    vector.length = vector.capacity = count;
    vector.itemSize = sizeof(*items);
    if (p->public.startElementHandler(p->public.UserData, element.uri, element.local, element.qname, &vector) != XML_OK)
        abort_callback(p, ERR_XMLP_ABORT);
    goto cleanup;
memory_error:
    abort_callback(p, ERR_XMLP_MEMORY_ALLOC);
cleanup:
    for (i = 0; i < built; ++i) free(names[i].storage);
    free(names);
    free(items);
    free(element.storage);
}

static void XMLCALL end_element(void *userdata, const char *name)
{
    Parser *p = (Parser *)userdata;
    Name element;
    if (!p->public.endElementHandler) return;
    if (!make_name(p, name, &element)) { abort_callback(p, ERR_XMLP_MEMORY_ALLOC); return; }
    if (p->public.endElementHandler(p->public.UserData, element.uri, element.local, element.qname) != XML_OK)
        abort_callback(p, ERR_XMLP_ABORT);
    free(element.storage);
}

static void XMLCALL characters(void *userdata, const char *text, int size)
{
    Parser *p = (Parser *)userdata;
    if (p->public.charactersHandler && p->public.charactersHandler(p->public.UserData, (const XMLCH *)text, size) != XML_OK)
        abort_callback(p, ERR_XMLP_ABORT);
}
static void XMLCALL start_cdata(void *userdata)
{
    Parser *p = (Parser *)userdata;
    if (p->public.startCDATAHandler && p->public.startCDATAHandler(p->public.UserData) != XML_OK)
        abort_callback(p, ERR_XMLP_ABORT);
}
static void XMLCALL end_cdata(void *userdata)
{
    Parser *p = (Parser *)userdata;
    if (p->public.endCDATAHandler && p->public.endCDATAHandler(p->public.UserData) != XML_OK)
        abort_callback(p, ERR_XMLP_ABORT);
}
static void XMLCALL comment(void *userdata, const char *text)
{
    Parser *p = (Parser *)userdata;
    if (p->public.commentHandler && p->public.commentHandler(p->public.UserData, (const XMLCH *)text, (int)strlen(text)) != XML_OK)
        abort_callback(p, ERR_XMLP_ABORT);
}
static void XMLCALL instruction(void *userdata, const char *target, const char *data)
{
    Parser *p = (Parser *)userdata;
    if (p->public.processingInstructionHandler && p->public.processingInstructionHandler(p->public.UserData,
            (const XMLCH *)target, (const XMLCH *)data) != XML_OK)
        abort_callback(p, ERR_XMLP_ABORT);
}
static int XMLCALL external_entity(XML_Parser parser, const char *context, const char *base,
                                   const char *system_id, const char *public_id)
{
    (void)parser; (void)context; (void)base; (void)system_id; (void)public_id;
    /* Layout/SVG parsing has no external-resource resolver. */
    return XML_STATUS_ERROR;
}

LPXMLPARSER XMLParser_Create(LPXMLPARSER *out)
{
    Parser *p = (Parser *)calloc(1, sizeof(*p));
    if (p) p->public.XMLFlags = XMLFLAG_NAMESPACES | XMLFLAG_CONVERT_EOL;
    if (out) *out = p ? &p->public : NULL;
    return p ? &p->public : NULL;
}
void XMLParser_Free(LPXMLPARSER parser)
{
    if (!parser) return;
    if (((Parser *)parser)->expat) XML_ParserFree(((Parser *)parser)->expat);
    free(parser);
}
void *XMLVector_Get(LPXMLVECTOR vector, int index)
{
    if (!vector || index < 0 || index >= vector->length) return NULL;
    return vector->array + (size_t)index * (size_t)vector->itemSize;
}
int XMLParser_GetCurrentLine(LPXMLPARSER parser)
{
    return parser && ((Parser *)parser)->expat ? (int)XML_GetCurrentLineNumber(((Parser *)parser)->expat) : 0;
}
int XMLParser_GetCurrentColumn(LPXMLPARSER parser)
{
    return parser && ((Parser *)parser)->expat ? (int)XML_GetCurrentColumnNumber(((Parser *)parser)->expat) + 1 : 0;
}
int XMLParser_Parse(LPXMLPARSER parser, LPFNINPUTSRC input, void *userdata, const XMLCH *encoding)
{
    Parser *p = (Parser *)parser;
    const char *error = NULL;
    BYTE buffer[4096];
    if (!p || !input) return 0;
    if (p->expat) XML_ParserFree(p->expat);
    p->expat = (parser->XMLFlags & XMLFLAG_NAMESPACES)
        ? XML_ParserCreateNS((const char *)encoding, '\x1f') : XML_ParserCreate((const char *)encoding);
    parser->ErrorCode = p->callback_aborted = 0;
    parser->ErrorString[0] = 0;
    if (!p->expat) { parser->ErrorCode = ERR_XMLP_MEMORY_ALLOC; error = "Out of memory"; goto failed; }
    XML_SetUserData(p->expat, p);
    XML_SetReturnNSTriplet(p->expat, 1);
    XML_SetElementHandler(p->expat, start_element, end_element);
    XML_SetCharacterDataHandler(p->expat, characters);
    XML_SetCdataSectionHandler(p->expat, start_cdata, end_cdata);
    XML_SetCommentHandler(p->expat, comment);
    XML_SetProcessingInstructionHandler(p->expat, instruction);
    XML_SetExternalEntityRefHandler(p->expat, external_entity);
    XML_SetParamEntityParsing(p->expat, XML_PARAM_ENTITY_PARSING_NEVER);
    if (parser->startDocumentHandler && parser->startDocumentHandler(parser->UserData) != XML_OK)
        p->callback_aborted = ERR_XMLP_ABORT;
    while (!p->callback_aborted) {
        int actual = 0, rc = input(buffer, sizeof(buffer), &actual, userdata);
        int final = rc == BIS_EOF || (rc == 0 && actual == 0);
        if (rc < 0 || rc > BIS_EOF || actual < 0 || actual > (int)sizeof(buffer)) {
            parser->ErrorCode = ERR_XMLP_READER_FATAL; error = "XML input reader failed"; goto failed;
        }
        if (XML_Parse(p->expat, (const char *)buffer, actual, final) != XML_STATUS_OK) {
            parser->ErrorCode = p->callback_aborted ? p->callback_aborted : ERR_XMLP_INVALID_TOKEN;
            error = XML_ErrorString(XML_GetErrorCode(p->expat)); goto failed;
        }
        if (final) {
            if (parser->endDocumentHandler && parser->endDocumentHandler(parser->UserData) != XML_OK) break;
            return 1;
        }
    }
    parser->ErrorCode = ERR_XMLP_ABORT;
    error = "XML parsing aborted by callback";
failed:
    parser->ErrorLine = XMLParser_GetCurrentLine(parser);
    parser->ErrorColumn = XMLParser_GetCurrentColumn(parser);
    snprintf((char *)parser->ErrorString, sizeof(parser->ErrorString), "%s", error ? error : "XML parse error");
    if (parser->errorHandler) parser->errorHandler(parser);
    return 0;
}
