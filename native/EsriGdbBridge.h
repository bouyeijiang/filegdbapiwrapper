//
// EsriGdbBridge.h
//
// Internal interface shared by the translation units of the native bridge.
// The bridge is split so that no single file becomes unmanageable:
//
//   EsriGdbNative.cpp          the public extern "C" ABI (the only exported
//                              entry points, see EsriGdbNative.h)
//   EsriGdbBridgeUtil.cpp      UTF-8 / base64 / error reporting / value
//                              conversion / catalog paths / C marshalling
//   EsriGdbBridgeXml.cpp       XML normalisation + table definition validation
//   EsriGdbBridgeRegistry.cpp  handle tables, object lifetime, field metadata
//
// This header is private to those files: it is neither installed nor part of
// the C ABI. Everything declared here lives in namespace esrigdb_bridge.
//
#ifndef ESRIGDB_BRIDGE_H
#define ESRIGDB_BRIDGE_H

#include "EsriGdbNative.h"

// Esri's public headers spell the import attribute with a single underscore on
// Windows ("#define EXT_FILEGDB_API _declspec(dllimport)"), which MSVC does
// not know under that spelling. Map it onto the real keyword before their
// headers are pulled in; harmless when the compiler happens to accept it.
#if defined(_WIN32) && !defined(_declspec)
#  define _declspec __declspec
#endif

#include <FileGDBAPI.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <vector>

// Convenience for the bridge translation units only - this is an internal
// header, so pulling in the Esri namespace here does not leak to callers.
using namespace FileGDBAPI;

// ---------------------------------------------------------------------------
// Opaque handle definitions
//
// Both types are declared opaque in EsriGdbNative.h; their members are only
// ever touched by the bridge itself.
// ---------------------------------------------------------------------------

// The opaque cursor declared as `struct egdb_cursor_t` in EsriGdbNative.h.
// It keeps the FileGDB enumerator open so rows can be pulled one at a time.
struct egdb_cursor_t
{
  std::mutex                mutex;          // serialises access to this cursor
  Geodatabase*              geodatabase;    // owner, not owned by the cursor
  egdb_tableobj*            ownerTable;     // NULL for SQL cursors
  egdb_handle               ownerHandle;
  EnumRows                   enumRows;       // the live query
  Row                        row;            // recycled by Next()
  std::vector<std::string>   fieldNames;     // UTF-8
  std::vector<int32_t>       fieldTypes;     // FieldType values
  std::vector<int32_t>       fieldLengths;
  std::vector<int32_t>       fieldNullable;
  bool                       hasResultSet;
  bool                       ended;
  bool                       closed;

  egdb_cursor_t()
    : geodatabase(NULL), ownerTable(NULL), ownerHandle(0),
      hasResultSet(false), ended(false), closed(false)
  {
  }
};

// The opaque table handle declared as `struct egdb_tableobj_t` in the header.
struct egdb_tableobj_t
{
  std::mutex                mutex;
  Geodatabase*              geodatabase;    // owner, not owned by the table
  Table*                    table;
  egdb_handle               gdbHandle;
  std::wstring              path;

  // Cached field metadata (refreshed after schema changes).
  std::vector<std::string>  fieldNames;
  std::vector<int32_t>      fieldTypes;
  std::vector<int32_t>      fieldLengths;
  std::vector<int32_t>      fieldNullable;

  bool                      closed;

  egdb_tableobj_t()
    : geodatabase(NULL), table(NULL), gdbHandle(0), closed(false)
  {
  }
};

// ---------------------------------------------------------------------------
// Shared state and helpers (defined in the EsriGdbBridge*.cpp files)
// ---------------------------------------------------------------------------
namespace esrigdb_bridge {

// --- version -----------------------------------------------------------------
extern const char* const kVersion;

// --- registries (EsriGdbBridgeRegistry.cpp) ---------------------------------
// The registry lock is held for whole operations (open / search / write) so
// that handles cannot be destroyed while they are in use. Documented rule:
// never take a handle's own mutex while already holding it, and never take
// g_mutex while holding a handle mutex.
extern std::mutex                           g_mutex;
extern std::map<egdb_handle, Geodatabase*>  g_geodatabases;
extern egdb_handle                          g_nextHandle;

// All live cursors, plus an index by owning geodatabase / table so that
// closing them can tear down the cursors still streaming from them.
extern std::set<egdb_cursor*>                               g_liveCursors;
extern std::map<egdb_handle, std::vector<egdb_cursor*> >    g_cursorsByGdb;
extern std::map<egdb_tableobj*, std::vector<egdb_cursor*> > g_cursorsByTable;

// All live tables, indexed by owning geodatabase.
extern std::set<egdb_tableobj*>                             g_liveTables;
extern std::map<egdb_handle, std::vector<egdb_tableobj*> >  g_tablesByGdb;

Geodatabase* FindGeodatabase(egdb_handle handle);

/// Destroys a cursor. Caller must hold g_mutex; the cursor's own mutex is
/// taken while the query is torn down.
void DestroyCursorLocked(egdb_cursor* cursor);

/// Destroys a table: first its cursors, then the FileGDB table itself.
/// Caller must hold g_mutex.
void DestroyTableLocked(egdb_tableobj* tableObj);

/// True when the pointer refers to a cursor that is still open.
bool IsLiveCursor(egdb_cursor* cursor);

/// Reads the column metadata of an enumerator into the cursor.
/// Returns false when the metadata cannot be read.
bool LoadCursorMetadata(egdb_cursor* cursor);

/// Publishes a cursor into the registries. Caller must hold g_mutex.
void RegisterCursorLocked(egdb_cursor* cursor);

/// (Re)loads the cached field metadata of a table. Caller holds table->mutex.
bool RefreshFieldCache(egdb_tableobj* tableObj);

/// Allocates a table handle and registers it. Caller must hold g_mutex.
egdb_result PublishTable(Geodatabase* geodatabase, Table* table,
                         egdb_handle handle, const std::wstring& path,
                         egdb_tableobj** outTable,
                         char* errBuf, int32_t errBufLen);

// --- encoding / errors / values (EsriGdbBridgeUtil.cpp) ---------------------
std::wstring Utf8ToWide(const char* utf8);
void         AppendUtf8(std::string& out, unsigned int cp);
std::string  WideToUtf8(const std::wstring& wide);

std::string  Base64Encode(const unsigned char* data, size_t len);
int          Base64Value(unsigned char c);
bool         Base64Decode(const char* text, std::vector<unsigned char>& out);

egdb_result  SetLastError(char* errBuf, int32_t errBufLen, fgdbError hr, const char* context);
egdb_result  SetNativeError(char* errBuf, int32_t errBufLen, egdb_result code, const char* context);
egdb_result  SetArgumentError(char* errBuf, int32_t errBufLen, const char* context);

bool         FormatNumber(double value, int significantDigits, std::string& out);

/// Extracts the value of the given field as UTF-8 text.
/// Returns false when the value could not be read (read error).
bool         ExtractValue(Row& row, int fieldNumber, FieldType fieldType,
                          std::string& out, bool& isNull);

bool         ParseInt16(const char* text, short& out);
bool         ParseInt32(const char* text, int32& out);
bool         ParseFloat(const char* text, float& out);
bool         ParseDouble(const char* text, double& out);
bool         ParseDate(const char* text, tm& out);

/// Number of Unicode characters (UTF-8 code points) in a UTF-8 string.
size_t       Utf8CharCount(const char* text);
const char*  FieldTypeName(FieldType fieldType);

/// Converts a UTF-8 text value to `fieldType` and stores it in the row.
/// `utf8Value` equal to NULL means SQL NULL.
/// Returns false when the value cannot be converted or the write fails; on
/// failure `errorText` receives a description and `hr` the fgdbError (S_OK for
/// pure conversion problems).
bool         ApplyValue(Row& row, const std::wstring& fieldName, FieldType fieldType,
                        const char* utf8Value, fgdbError& hr, std::string& errorText);

/// Dataset paths in the catalog always start with a backslash, e.g. "\Cities".
std::wstring RootedPath(const char* utf8Path);

/// Case insensitive ASCII comparison (FileGDB field names are not case sensitive).
bool         EqualsIgnoreCase(const std::string& a, const char* b);

/// Index of the field with the given UTF-8 name, or -1 when unknown.
int          FindFieldIndex(const egdb_tableobj* tableObj, const char* utf8Name);

Envelope     ToEnvelope(const egdb_envelope* envelope);
bool         HasEnvelope(const egdb_envelope* envelope);

char*        DupString(const std::string& value);
char**       DupPointerArray(const std::vector<char*>& list);
int32_t*     DupIntArray(const std::vector<int32_t>& list);
void         FreeStrings(std::vector<char*>& list);

/// Moves UTF-8 strings into an egdb_string_array (the array owns them).
void         FillStringArray(const std::vector<std::string>& items, egdb_string_array* out);
void         InitTable(egdb_table* table);

// --- XML (EsriGdbBridgeXml.cpp) ---------------------------------------------
/// Start tag of an element inside an XML document.
struct XmlTag
{
  size_t nameStart;   // index of the first character of the element name
  size_t nameEnd;     // index one past the element name
  size_t tagEnd;      // index of the '>' terminating the start tag
  std::string name;   // the element name, prefix included
};

/// A direct child of the root element.
struct XmlChild
{
  std::string name;     // element name, prefix included
  size_t      start;    // index of its '<'
};

/// Locates the root element of `xml`. Returns false when xml has no element.
bool FindRootTag(const std::string& xml, XmlTag& tag);

/// Returns the part of an element name after its prefix ("esri:Field" -> "Field").
std::string LocalName(const std::string& name);

/// True when the start tag carries a namespace declaration for `decl`
/// (e.g. "xmlns:xsi"), honouring both ' and " quoting.
bool HasNamespaceDecl(const std::string& xml, size_t from, size_t length, const char* decl);

/// Rewrites the root element (and its closing tag) to `newName` when needed
/// and adds the esri/xsi/xs namespace declarations that are still missing.
/// @param[in] newName  NULL keeps the local name and only adds the esri prefix.
void NormalizeXmlFragment(std::string& xml, const char* newName);

/// Collects the direct children of the root element.
void CollectRootChildren(const std::string& xml, std::vector<XmlChild>& children);

/// True when `xml` is a table definition the FileGDB API can safely consume;
/// otherwise `problem` describes what has to change.
bool ValidateTableDefinition(const std::string& xml, std::string& problem);

} // namespace esrigdb_bridge

#endif // ESRIGDB_BRIDGE_H
