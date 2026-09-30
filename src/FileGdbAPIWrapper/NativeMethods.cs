using System.Runtime.InteropServices;
using System.Text;

namespace EsriGdb;

/// <summary>
/// P/Invoke surface of <c>esrigdbnative</c> - the C ABI bridge
/// (<c>libesrigdbnative.so</c> / <c>esrigdbnative.dll</c>) that talks to the
/// Esri FileGDB API. All text crossing this boundary is UTF-8.
/// </summary>
internal static class NativeMethods
{
    internal const string LibraryName = "esrigdbnative";

    /// <summary>
    /// Platform specific guidance appended to load failures. The bare name
    /// resolves to <c>esrigdbnative.dll</c> on Windows and
    /// <c>libesrigdbnative.so</c> on Linux/macOS.
    /// </summary>
    internal static string LoadFailureHint =>
        OperatingSystem.IsWindows()
            ? "Build native/ with CMake ('cmake -S native -B native/build-win' followed by " +
              "'cmake --build native/build-win --config Release'), or drop a prebuilt " +
              "esrigdbnative.dll into lib/, and make sure esrigdbnative.dll + FileGDBAPI.dll " +
              "sit next to the application assembly."
            : "Build the native/ folder ('make') and make sure libesrigdbnative.so, " +
              "libFileGDBAPI.so and libfgdbunixrtl.so sit next to the application assembly.";

    internal const int ErrorBufferSize = 2048;

    /// <summary>
    /// Mirrors <c>egdb_table</c> in native/EsriGdbNative.h.
    /// Row-major: <c>values[row * field_count + column]</c>; a NULL pointer is SQL NULL.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct EgdbTable
    {
        public int FieldCount;
        public int RowCount;
        public IntPtr FieldNames;
        public IntPtr FieldTypes;
        public IntPtr Values;
    }

    /// <summary>Mirrors <c>egdb_row</c> in native/EsriGdbNative.h.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct EgdbRow
    {
        public int FieldCount;
        public IntPtr Values;
    }

    /// <summary>egdb_cursor_next(): outRow holds a row.</summary>
    internal const int CursorRow = 0;

    /// <summary>egdb_cursor_next(): the result set is exhausted.</summary>
    internal const int CursorEnd = 1;

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_open")]
    internal static extern int Open(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path,
        out long outHandle,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_close")]
    internal static extern int Close(long handle, [Out] byte[] errorBuffer, int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_execute_sql")]
    internal static extern int ExecuteSql(
        long handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Sql,
        out EgdbTable outTable,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_free_table")]
    internal static extern void FreeTable(ref EgdbTable table);

    // ---------------------------------------------------------------------
    // Cursor (streaming) API
    // ---------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_execute_sql_cursor")]
    internal static extern int ExecuteSqlCursor(
        long handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Sql,
        out IntPtr outCursor,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_cursor_next")]
    internal static extern int CursorNext(IntPtr cursor, out EgdbRow outRow, [Out] byte[] errorBuffer, int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_cursor_field_count")]
    internal static extern int CursorFieldCount(IntPtr cursor);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_cursor_field_name")]
    internal static extern IntPtr CursorFieldName(IntPtr cursor, int index);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_cursor_field_type")]
    internal static extern int CursorFieldType(IntPtr cursor, int index);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_cursor_field_length")]
    internal static extern int CursorFieldLength(IntPtr cursor, int index);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_cursor_field_nullable")]
    internal static extern int CursorFieldNullable(IntPtr cursor, int index);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_cursor_close")]
    internal static extern void CursorClose(IntPtr cursor);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_free_row")]
    internal static extern void FreeRow(ref EgdbRow row);

    // ---------------------------------------------------------------------
    // Strings / string arrays / envelopes
    // ---------------------------------------------------------------------

    /// <summary>Mirrors <c>egdb_string_array</c> in native/EsriGdbNative.h.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct EgdbStringArray
    {
        public int Count;
        public IntPtr Items;
    }

    /// <summary>Mirrors <c>egdb_envelope</c> in native/EsriGdbNative.h.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct EgdbEnvelope
    {
        public int UseEnvelope;
        public double XMin;
        public double YMin;
        public double XMax;
        public double YMax;

        internal static EgdbEnvelope From(Envelope? envelope) => envelope is { } box
            ? new EgdbEnvelope { UseEnvelope = 1, XMin = box.XMin, YMin = box.YMin, XMax = box.XMax, YMax = box.YMax }
            : default;
    }

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_free_string")]
    internal static extern void FreeString(IntPtr str);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_free_string_array")]
    internal static extern void FreeStringArray(ref EgdbStringArray array);

    /// <summary>Turns a native <c>char**</c> array into a managed array.</summary>
    internal static string[] ReadStringArray(EgdbStringArray array)
    {
        int count = Math.Max(array.Count, 0);
        var result = new string[count];

        for (int i = 0; i < count; i++)
        {
            IntPtr item = array.Items == IntPtr.Zero
                ? IntPtr.Zero
                : Marshal.ReadIntPtr(array.Items, i * IntPtr.Size);
            result[i] = item == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(item) ?? string.Empty;
        }

        return result;
    }

    /// <summary>Reads (and releases) a string returned through a <c>char** out</c> parameter.</summary>
    internal static string TakeString(IntPtr pointer)
    {
        if (pointer == IntPtr.Zero)
        {
            return string.Empty;
        }

        try
        {
            return Marshal.PtrToStringUTF8(pointer) ?? string.Empty;
        }
        finally
        {
            NativeMethods.FreeString(pointer);
        }
    }

    // ---------------------------------------------------------------------
    // Geodatabase management
    // ---------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_create_geodatabase")]
    internal static extern int CreateGeodatabase(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path,
        out long outHandle,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_delete_geodatabase")]
    internal static extern int DeleteGeodatabase(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_compact_database")]
    internal static extern int CompactDatabase(long handle, [Out] byte[] errorBuffer, int errorBufferLength);

    // ---------------------------------------------------------------------
    // Catalog browsing
    // ---------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_get_dataset_types")]
    internal static extern int GetDatasetTypes(
        long handle, out EgdbStringArray outTypes, [Out] byte[] errorBuffer, int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_get_child_datasets")]
    internal static extern int GetChildDatasets(
        long handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8ParentPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8DatasetType,
        out EgdbStringArray outChildren,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_get_query_name")]
    internal static extern int GetQueryName(
        long handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path,
        out IntPtr outName,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_get_dataset_definition")]
    internal static extern int GetDatasetDefinition(
        long handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8DatasetType,
        out IntPtr outXml,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    // ---------------------------------------------------------------------
    // Domains
    // ---------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_get_domains")]
    internal static extern int GetDomains(
        long handle, out EgdbStringArray outNames, [Out] byte[] errorBuffer, int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_get_domain_definition")]
    internal static extern int GetDomainDefinition(
        long handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8DomainName,
        out IntPtr outXml,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_create_domain")]
    internal static extern int CreateDomain(
        long handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Xml,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_delete_domain")]
    internal static extern int DeleteDomain(
        long handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8DomainName,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    // ---------------------------------------------------------------------
    // Tables
    // ---------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_open_table")]
    internal static extern int OpenTable(
        long handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path,
        out IntPtr outTable,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_create_table")]
    internal static extern int CreateTable(
        long handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8TableDefXml,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8ParentPath,
        out IntPtr outTable,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_close_table")]
    internal static extern int CloseTable(
        long handle, IntPtr table, [Out] byte[] errorBuffer, int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_field_count")]
    internal static extern int TableFieldCount(IntPtr table);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_field_name")]
    internal static extern IntPtr TableFieldName(IntPtr table, int index);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_field_type")]
    internal static extern int TableFieldType(IntPtr table, int index);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_field_length")]
    internal static extern int TableFieldLength(IntPtr table, int index);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_field_nullable")]
    internal static extern int TableFieldNullable(IntPtr table, int index);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_get_definition")]
    internal static extern int TableGetDefinition(
        IntPtr table, out IntPtr outXml, [Out] byte[] errorBuffer, int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_add_field")]
    internal static extern int TableAddField(
        IntPtr table,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8FieldDefXml,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_delete_field")]
    internal static extern int TableDeleteField(
        IntPtr table,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8FieldName,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_add_index")]
    internal static extern int TableAddIndex(
        IntPtr table,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8IndexDefXml,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_search")]
    internal static extern int TableSearch(
        IntPtr table,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Subfields,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8WhereClause,
        in EgdbEnvelope envelope,
        int recycling,
        out IntPtr outCursor,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_insert")]
    internal static extern int TableInsert(
        IntPtr table,
        [In] IntPtr[] names,
        [In] IntPtr[] values,
        int count,
        out int outNewOid,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_update")]
    internal static extern int TableUpdate(
        IntPtr table,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8WhereClause,
        in EgdbEnvelope envelope,
        [In] IntPtr[] names,
        [In] IntPtr[] values,
        int count,
        out int outAffected,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_table_delete")]
    internal static extern int TableDelete(
        IntPtr table,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8WhereClause,
        in EgdbEnvelope envelope,
        out int outAffected,
        [Out] byte[] errorBuffer,
        int errorBufferLength);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "egdb_version")]
    internal static extern IntPtr VersionPtr();

    internal static string Version()
    {
        IntPtr ptr = VersionPtr();
        return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
    }

    internal static byte[] NewErrorBuffer() => new byte[ErrorBufferSize];

    /// <summary>Decodes the UTF-8 error buffer written by the native bridge.</summary>
    internal static string ReadError(byte[] errorBuffer)
    {
        int length = Array.IndexOf(errorBuffer, (byte)0);
        if (length < 0)
        {
            length = errorBuffer.Length;
        }

        return length == 0
            ? string.Empty
            : Encoding.UTF8.GetString(errorBuffer, 0, length).Trim();
    }

    /// <summary>Throws <see cref="EsriGdbException"/> when the native call failed.</summary>
    internal static void ThrowOnError(int errorCode, byte[] errorBuffer, string operation)
    {
        if (errorCode == 0)
        {
            return;
        }

        string detail = ReadError(errorBuffer);
        string message = string.IsNullOrEmpty(detail)
            ? $"{operation} failed (error code {errorCode})."
            : $"{operation} failed: {detail}";

        throw new EsriGdbException(message, errorCode);
    }
}
