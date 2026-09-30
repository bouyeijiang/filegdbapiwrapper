//
// EsriGdbNative.h
//
// Plain C ABI bridge over the Esri FileGDB API (libFileGDBAPI.so).
// This is the layer that .NET (or any other FFI capable language) talks to.
//
// Conventions:
//   * All text crossing the boundary is UTF-8.
//   * Functions return an fgdbError code (0 == S_OK); on failure an optional
//     caller supplied buffer receives a human readable description.
//   * All memory returned through egdb_table is allocated by the library and
//     must be released with egdb_free_table().
//
#pragma once

#include <stdint.h>

#if defined(_WIN32)
#  ifdef ESGDB_NATIVE_EXPORTS
#    define EGDB_EXPORT __declspec(dllexport)
#  else
#    define EGDB_EXPORT __declspec(dllimport)
#  endif
#else
#  define EGDB_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Result code. 0 (S_OK) means success, anything else is an fgdbError value.
typedef int32_t egdb_result;

/// Opaque handle identifying an open geodatabase.
typedef int64_t egdb_handle;

#define EGDB_OK ((egdb_result)0)

/// Returned for malformed arguments / invalid handles (deliberately distinct
/// from any fgdbError value produced by the FileGDB API).
#define EGDB_E_INVALIDARG ((egdb_result)-1)

/// Returned when the bridge itself failed (out of memory, read error, ...).
#define EGDB_E_FAIL ((egdb_result)-2)

/// A rectangular result set: field_count columns by row_count rows.
/// Layout is row major: values[row * field_count + column].
/// A value pointer equal to NULL means SQL NULL.
typedef struct egdb_table_t
{
  int32_t   field_count;
  int32_t   row_count;
  char**    field_names;   // UTF-8, field_count entries
  int32_t*  field_types;   // FieldType values, field_count entries
  char**    values;        // UTF-8, field_count * row_count entries (NULL == SQL NULL)
} egdb_table;

/// Opens a file geodatabase.
/// @param[in]  utf8Path   Path to the .gdb folder (UTF-8).
/// @param[out] outHandle  Receives the handle of the open geodatabase.
/// @param[out] errBuf     Optional, receives an error description on failure.
/// @param[in]  errBufLen  Size of errBuf in bytes.
EGDB_EXPORT egdb_result egdb_open(const char* utf8Path, egdb_handle* outHandle,
                                  char* errBuf, int32_t errBufLen);

/// Closes a geodatabase previously opened with egdb_open().
EGDB_EXPORT egdb_result egdb_close(egdb_handle handle, char* errBuf, int32_t errBufLen);

/// Executes a SQL statement (typically SELECT) and returns the whole result set.
/// @param[in]  handle    Handle of an open geodatabase.
/// @param[in]  utf8Sql   SQL statement (UTF-8).
/// @param[out] outTable  Receives the result set; release with egdb_free_table().
///                        Statements that produce no rows yield an empty table.
EGDB_EXPORT egdb_result egdb_execute_sql(egdb_handle handle, const char* utf8Sql,
                                         egdb_table* outTable, char* errBuf, int32_t errBufLen);

/// Releases every allocation owned by the table and zeroes the structure.
EGDB_EXPORT void egdb_free_table(egdb_table* table);

// ---------------------------------------------------------------------------
// Cursor (streaming) API - consumes the result set row by row instead of
// materialising it in memory.
// ---------------------------------------------------------------------------

/// Returned by egdb_cursor_next(): a row has been placed in outRow.
#define EGDB_CURSOR_ROW ((egdb_result)0)
/// Returned by egdb_cursor_next(): no more rows (end of the result set).
#define EGDB_CURSOR_END ((egdb_result)1)

/// A single row of a cursor. values holds field_count UTF-8 pointers,
/// a NULL pointer means SQL NULL. Release with egdb_free_row().
typedef struct egdb_row_t
{
  int32_t  field_count;
  char**   values;        // UTF-8, field_count entries (NULL == SQL NULL)
} egdb_row;

/// Opaque streaming cursor, valid until egdb_cursor_close().
typedef struct egdb_cursor_t egdb_cursor;

/// Executes a SQL statement and returns a cursor over its result set.
/// The query stays open until egdb_cursor_close(), so the geodatabase must not
/// be closed while one of its cursors is still alive (egdb_close() closes any
/// remaining cursor of that geodatabase automatically).
/// @param[in]  handle    Handle of an open geodatabase.
/// @param[in]  utf8Sql   SQL statement (UTF-8).
/// @param[out] outCursor Receives the cursor; release with egdb_cursor_close().
EGDB_EXPORT egdb_result egdb_execute_sql_cursor(egdb_handle handle, const char* utf8Sql,
                                                egdb_cursor** outCursor,
                                                char* errBuf, int32_t errBufLen);

/// Fetches the next row of the cursor.
/// @return EGDB_CURSOR_ROW (0) - outRow holds a row,
///         EGDB_CURSOR_END (1) - the result set is exhausted,
///         anything else       - an error occurred (see errBuf).
EGDB_EXPORT egdb_result egdb_cursor_next(egdb_cursor* cursor, egdb_row* outRow,
                                         char* errBuf, int32_t errBufLen);

/// Number of columns of the cursor's result set (0 for statements without a
/// result set). Read it right after egdb_execute_sql_cursor().
EGDB_EXPORT int32_t egdb_cursor_field_count(const egdb_cursor* cursor);

/// UTF-8 column name owned by the cursor (valid until egdb_cursor_close()).
/// Returns NULL when the index is out of range.
EGDB_EXPORT const char* egdb_cursor_field_name(const egdb_cursor* cursor, int32_t index);

/// FieldType of the column, or -1 when the index is out of range.
EGDB_EXPORT int32_t egdb_cursor_field_type(const egdb_cursor* cursor, int32_t index);

/// Field length in characters of the column (-1 unknown, -2 out of range).
EGDB_EXPORT int32_t egdb_cursor_field_length(const egdb_cursor* cursor, int32_t index);

/// 1 when the column accepts NULL, 0 when it does not, -1 out of range.
EGDB_EXPORT int32_t egdb_cursor_field_nullable(const egdb_cursor* cursor, int32_t index);

/// Releases the cursor (and its query). Safe to call with NULL, and idempotent
/// for a valid cursor.
EGDB_EXPORT void egdb_cursor_close(egdb_cursor* cursor);

/// Releases the row returned by egdb_cursor_next() and zeroes the structure.
EGDB_EXPORT void egdb_free_row(egdb_row* row);

// ---------------------------------------------------------------------------
// Common helpers
// ---------------------------------------------------------------------------

/// A NULL terminated array of UTF-8 strings returned by the library.
typedef struct egdb_string_array_t
{
  int32_t  count;
  char**   items;
} egdb_string_array;

/// Releases a string returned through a `char** out` parameter.
EGDB_EXPORT void egdb_free_string(char* str);

/// Releases an egdb_string_array (items and container).
EGDB_EXPORT void egdb_free_string_array(egdb_string_array* array);

/// Optional spatial filter. Set use_envelope to 0 (or pass NULL where an
/// `const egdb_envelope*` is accepted) to search without a spatial constraint.
typedef struct egdb_envelope_t
{
  int32_t  use_envelope;
  double   xmin;
  double   ymin;
  double   xmax;
  double   ymax;
} egdb_envelope;

// ---------------------------------------------------------------------------
// Geodatabase management (CreateGeodatabase / DeleteGeodatabase / Compact)
// ---------------------------------------------------------------------------

/// Creates a new file geodatabase and returns it open (mirror of the
/// GeodatabaseManagement sample).
EGDB_EXPORT egdb_result egdb_create_geodatabase(const char* utf8Path, egdb_handle* outHandle,
                                                char* errBuf, int32_t errBufLen);

/// Deletes the file geodatabase at utf8Path. It must not be open.
EGDB_EXPORT egdb_result egdb_delete_geodatabase(const char* utf8Path,
                                                char* errBuf, int32_t errBufLen);

/// Compacts the geodatabase (recommended after bulk updates).
EGDB_EXPORT egdb_result egdb_compact_database(egdb_handle handle,
                                              char* errBuf, int32_t errBufLen);

// ---------------------------------------------------------------------------
// Catalog browsing (dataset types / children / query name / definitions)
// ---------------------------------------------------------------------------

/// All dataset types of the geodatabase, e.g. "Feature Class", "Table".
EGDB_EXPORT egdb_result egdb_get_dataset_types(egdb_handle handle,
                                               egdb_string_array* outTypes,
                                               char* errBuf, int32_t errBufLen);

/// Children of a dataset (pass an empty parentPath for the root, an empty
/// datasetType for all types).
EGDB_EXPORT egdb_result egdb_get_child_datasets(egdb_handle handle,
                                                const char* utf8ParentPath,
                                                const char* utf8DatasetType,
                                                egdb_string_array* outChildren,
                                                char* errBuf, int32_t errBufLen);

/// The name to use for a dataset inside SQL statements.
EGDB_EXPORT egdb_result egdb_get_query_name(egdb_handle handle, const char* utf8Path,
                                            char** outName,
                                            char* errBuf, int32_t errBufLen);

/// The XML data element definition of a dataset (datasetType e.g. "Table").
EGDB_EXPORT egdb_result egdb_get_dataset_definition(egdb_handle handle,
                                                    const char* utf8Path,
                                                    const char* utf8DatasetType,
                                                    char** outXml,
                                                    char* errBuf, int32_t errBufLen);

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------

/// Names of all domains defined in the geodatabase.
EGDB_EXPORT egdb_result egdb_get_domains(egdb_handle handle,
                                         egdb_string_array* outNames,
                                         char* errBuf, int32_t errBufLen);

/// XML definition of a domain (range domain or coded value domain).
EGDB_EXPORT egdb_result egdb_get_domain_definition(egdb_handle handle,
                                                   const char* utf8DomainName,
                                                   char** outXml,
                                                   char* errBuf, int32_t errBufLen);

/// Creates a domain from an XML definition.
EGDB_EXPORT egdb_result egdb_create_domain(egdb_handle handle, const char* utf8Xml,
                                           char* errBuf, int32_t errBufLen);

/// Deletes a domain by name.
EGDB_EXPORT egdb_result egdb_delete_domain(egdb_handle handle, const char* utf8DomainName,
                                           char* errBuf, int32_t errBufLen);

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

/// Opaque handle to an open table / feature class of a geodatabase.
typedef struct egdb_tableobj_t egdb_tableobj;

/// Opens a table by its path/query name (e.g. "Cities" or "\\Cities").
EGDB_EXPORT egdb_result egdb_open_table(egdb_handle handle, const char* utf8Path,
                                        egdb_tableobj** outTable,
                                        char* errBuf, int32_t errBufLen);

/// Creates a table / feature class from an XML data element definition
/// (mirror of Geodatabase::CreateTable(tableDef, parent, table)).
/// The definition must be a complete <esri:DataElement>: CatalogPath, Name,
/// DatasetType, HasOID, Fields, Indexes and CLSID, with <Indexes> before
/// <CLSID>. Missing pieces are reported as EGDB_E_INVALIDARG - without them
/// the underlying Esri parser would either fail with a generic error or
/// crash the process. Prefixes/namespaces are normalised first (see below).
/// @param[in] utf8ParentPath  Feature dataset path, or "" for the root.
EGDB_EXPORT egdb_result egdb_create_table(egdb_handle handle,
                                          const char* utf8TableDefXml,
                                          const char* utf8ParentPath,
                                          egdb_tableobj** outTable,
                                          char* errBuf, int32_t errBufLen);

/// Closes a table previously opened/created on this geodatabase.
EGDB_EXPORT egdb_result egdb_close_table(egdb_handle handle, egdb_tableobj* table,
                                         char* errBuf, int32_t errBufLen);

/// Number of fields of the table (0 when the table is closed/invalid).
EGDB_EXPORT int32_t egdb_table_field_count(egdb_tableobj* table);
/// UTF-8 field name owned by the table, NULL when the index is out of range.
EGDB_EXPORT const char* egdb_table_field_name(egdb_tableobj* table, int32_t index);
/// FieldType, or -1 when out of range.
EGDB_EXPORT int32_t egdb_table_field_type(egdb_tableobj* table, int32_t index);
/// Field length in characters (-1 unknown, -2 out of range).
EGDB_EXPORT int32_t egdb_table_field_length(egdb_tableobj* table, int32_t index);
/// 1 when nullable, 0 when not, -1 out of range.
EGDB_EXPORT int32_t egdb_table_field_nullable(egdb_tableobj* table, int32_t index);

/// XML definition of the table (schema).
EGDB_EXPORT egdb_result egdb_table_get_definition(egdb_tableobj* table, char** outXml,
                                                  char* errBuf, int32_t errBufLen);

/// Adds a field from an XML field definition.
EGDB_EXPORT egdb_result egdb_table_add_field(egdb_tableobj* table, const char* utf8FieldDefXml,
                                             char* errBuf, int32_t errBufLen);

/// Deletes a field by name.
EGDB_EXPORT egdb_result egdb_table_delete_field(egdb_tableobj* table, const char* utf8FieldName,
                                                char* errBuf, int32_t errBufLen);

/// Adds an index from an XML index definition.
EGDB_EXPORT egdb_result egdb_table_add_index(egdb_tableobj* table, const char* utf8IndexDefXml,
                                             char* errBuf, int32_t errBufLen);

/// Attribute (+ optional envelope) search. Streaming: the result is a cursor.
/// @param[in] utf8Subfields   Fields to return, "" or "*" for all.
/// @param[in] utf8WhereClause Predicate, "" for no filter.
/// @param[in] envelope        Optional spatial filter, NULL for none.
/// @param[in] recycling       1 = recycle memory (read only), 0 when the rows
///                            will be modified/updated by other calls.
EGDB_EXPORT egdb_result egdb_table_search(egdb_tableobj* table,
                                          const char* utf8Subfields,
                                          const char* utf8WhereClause,
                                          const egdb_envelope* envelope,
                                          int32_t recycling,
                                          egdb_cursor** outCursor,
                                          char* errBuf, int32_t errBufLen);

/// Inserts a new row. names[i] selects the field; values[i] is its UTF-8 text
/// value parsed according to the field type (NULL for SQL NULL); geometry and
/// blob values are base64 encoded. Fields that are not listed keep their
/// default value.
/// @param[out] outNewOid  Optional, receives the OBJECTID of the new row (-1
///                        when it could not be determined).
EGDB_EXPORT egdb_result egdb_table_insert(egdb_tableobj* table,
                                          const char* const* names,
                                          const char* const* values,
                                          int32_t count,
                                          int32_t* outNewOid,
                                          char* errBuf, int32_t errBufLen);

/// Updates every row matching the where clause / envelope with the given
/// values. An absent where clause and envelope match every row.
/// @param[out] outAffected  Optional, receives the number of rows updated.
EGDB_EXPORT egdb_result egdb_table_update(egdb_tableobj* table,
                                          const char* utf8WhereClause,
                                          const egdb_envelope* envelope,
                                          const char* const* names,
                                          const char* const* values,
                                          int32_t count,
                                          int32_t* outAffected,
                                          char* errBuf, int32_t errBufLen);

/// Deletes every row matching the where clause / envelope. An absent where
/// clause and envelope match every row.
/// @param[out] outAffected  Optional, receives the number of rows deleted.
EGDB_EXPORT egdb_result egdb_table_delete(egdb_tableobj* table,
                                          const char* utf8WhereClause,
                                          const egdb_envelope* envelope,
                                          int32_t* outAffected,
                                          char* errBuf, int32_t errBufLen);

/// Returns a static, NUL terminated version string of this bridge.
EGDB_EXPORT const char* egdb_version(void);

#ifdef __cplusplus
}
#endif
