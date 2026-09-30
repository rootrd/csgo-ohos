/* Regression coverage for the streaming SAX contract consumed by Panorama.
 * Compile the adapter with libparsifal's header and this test with Source's
 * public header, so their shared ABI is also exercised. */
#include <parsifal.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *xml;
    size_t position, chunk;
    int starts, ends, cdata, documents, errors, abort;
    char text[128];
    size_t text_length;
} Test;

static int read_xml(BYTE *buffer, int capacity, int *actual, void *userdata)
{
    Test *test = userdata;
    size_t remaining = strlen(test->xml) - test->position;
    size_t count = remaining < test->chunk ? remaining : test->chunk;
    if (count > (size_t)capacity) count = capacity;
    memcpy(buffer, test->xml + test->position, count);
    test->position += count;
    *actual = (int)count;
    return count == remaining ? BIS_EOF : 0;
}

static int start(void *userdata, const XMLCH *uri, const XMLCH *local,
                 const XMLCH *name, LPXMLVECTOR attributes)
{
    Test *test = userdata;
    ++test->starts;
    assert(XMLVector_Get(attributes, -1) == NULL);
    assert(XMLVector_Get(attributes, attributes->length) == NULL);
    if (!strcmp((const char *)name, "ui:Panel")) {
        assert(!strcmp((const char *)uri, "urn:ui"));
        assert(!strcmp((const char *)local, "Panel"));
        assert(attributes->length == 1);
        XMLRUNTIMEATT *attribute = XMLVector_Get(attributes, 0);
        assert(!strcmp((const char *)attribute->qname, "ui:id"));
        assert(!strcmp((const char *)attribute->uri, "urn:ui"));
        assert(!strcmp((const char *)attribute->value, "a&b"));
    }
    return test->abort ? XML_ABORT : XML_OK;
}

static int end(void *userdata, const XMLCH *uri, const XMLCH *local, const XMLCH *name)
{
    (void)uri; (void)local; (void)name;
    ++((Test *)userdata)->ends;
    return XML_OK;
}

static int characters(void *userdata, const XMLCH *text, int count)
{
    Test *test = userdata;
    assert(test->text_length + count < sizeof(test->text));
    memcpy(test->text + test->text_length, text, count);
    test->text_length += count;
    return XML_OK;
}

static int cdata(void *userdata) { ++((Test *)userdata)->cdata; return XML_OK; }
static int document(void *userdata) { ++((Test *)userdata)->documents; return XML_OK; }
static void error(LPXMLPARSER parser)
{
    ++((Test *)parser->UserData)->errors;
    assert(parser->ErrorCode != 0 && parser->ErrorString[0] != 0);
    assert(parser->ErrorLine > 0 && parser->ErrorColumn > 0);
}

static int parse(LPXMLPARSER parser, Test *test)
{
    parser->UserData = test;
    return XMLParser_Parse(parser, read_xml, test, (const XMLCH *)"UTF-8");
}

int main(void)
{
    LPXMLPARSER parser = NULL;
    assert(XMLParser_Create(&parser) == parser && parser);
    parser->startElementHandler = start;
    parser->endElementHandler = end;
    parser->charactersHandler = characters;
    parser->startCDATAHandler = cdata;
    parser->endDocumentHandler = document;
    parser->errorHandler = error;
    const char *xml = "<ui:Panel xmlns:ui='urn:ui' ui:id='a&amp;b'><Label/>"
                      "<![CDATA[<text>]]>&#x4E2D;</ui:Panel>";
    for (size_t chunk = 1; chunk <= 4096; chunk *= 2) {
        Test test = {.xml = xml, .chunk = chunk};
        assert(parse(parser, &test) == 1);
        assert(test.starts == 2 && test.ends == 2 && test.cdata == 1);
        assert(test.documents == 1 && test.errors == 0);
        assert(!strcmp(test.text, "<text>中"));
    }
    const char *invalid[] = {"", "<a>", "<a></b>", "<a x='1' x='2'/>",
                            "<a/><b/>", "<a>&#0;</a>", "<a>&unknown;</a>",
                            "<!DOCTYPE a [<!ENTITY e SYSTEM 'file:///unused'>]><a>&e;</a>"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        Test test = {.xml = invalid[i], .chunk = 1};
        assert(parse(parser, &test) == 0);
        assert(test.errors == 1 && test.documents == 0);
    }
    Test aborted = {.xml = "<Panel><Label/></Panel>", .chunk = 4096, .abort = 1};
    assert(parse(parser, &aborted) == 0);
    assert(aborted.starts == 1 && aborted.errors == 1 && aborted.documents == 0);
    // Reuse after an abort must clear the old error/parser state.
    Test reused = {.xml = "<Panel/>", .chunk = 1};
    assert(parse(parser, &reused) == 1 && parser->ErrorCode == 0);
    XMLParser_Free(parser);
    puts("Parsifal/Expat streaming, namespaces, errors and callback abort: PASS");
    return 0;
}
