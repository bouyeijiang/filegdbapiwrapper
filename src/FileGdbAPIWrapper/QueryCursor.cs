using System.Collections;

namespace EsriGdb;

/// <summary>
/// A streaming (cursor) view over a query result. Unlike
/// <see cref="Geodatabase.ExecuteSql"/>, rows are fetched from the native layer
/// one at a time, so arbitrarily large result sets can be processed with a
/// constant memory footprint.
///
/// <code>
/// using QueryCursor cursor = geodatabase.ExecuteSqlCursor("SELECT * FROM HugeTable");
/// while (cursor.ReadNext() is Row row)
/// {
///     Process(row);
/// }
/// </code>
///
/// The query stays open on the native side until the cursor is disposed.
/// A cursor is single pass: it can be enumerated (or read with
/// <see cref="ReadNext"/>) only once. Disposing the owning
/// <see cref="Geodatabase"/> or <see cref="Table"/> closes its remaining cursors.
/// </summary>
public sealed class QueryCursor : IEnumerable<Row>, IDisposable
{
    private readonly ICursorOwner _owner;
    private readonly Field[] _fields;
    private readonly Dictionary<string, int> _nameToIndex;

    private IntPtr _handle;
    private bool _ended;
    private bool _aborted;
    private bool _consumed;
    private bool _enumerationStarted;

    private QueryCursor(ICursorOwner owner, IntPtr handle)
    {
        _owner = owner;
        _handle = handle;
        _fields = CursorMarshal.ReadFields(handle);
        _nameToIndex = CursorMarshal.BuildNameMap(_fields);
    }

    /// <summary>
    /// Creates a cursor and registers it with its owner, so that closing the
    /// owner (geodatabase or table) also closes the cursor.
    /// </summary>
    internal static QueryCursor Attach(
        ICursorOwner owner, IntPtr handle, Func<bool> isStillOpen, string closedMessage)
    {
        var cursor = new QueryCursor(owner, handle);

        try
        {
            lock (owner.CursorSync)
            {
                if (!isStillOpen())
                {
                    // The owner vanished while the query was being set up.
                    throw new EsriGdbException(closedMessage, -1);
                }

                owner.AddCursor(cursor);
            }
        }
        catch
        {
            cursor.Dispose();
            throw;
        }

        return cursor;
    }

    /// <summary>Column metadata; available before the first row is read.</summary>
    public IReadOnlyList<Field> Fields => _fields;

    /// <summary>True once the result set has been read to the end (or disposed).</summary>
    public bool IsEnded => _ended || _handle == IntPtr.Zero;

    /// <summary>
    /// Fetches the next row, or <c>null</c> when the result set is exhausted.
    /// </summary>
    /// <exception cref="EsriGdbException">The row could not be read.</exception>
    /// <exception cref="InvalidOperationException">The cursor was already enumerated.</exception>
    public Row? ReadNext()
    {
        if (_consumed)
        {
            return null;
        }

        if (_enumerationStarted)
        {
            throw new InvalidOperationException(
                "This cursor is being read through foreach; use the foreach loop instead of ReadNext().");
        }

        Row? row = ReadCore();
        if (row is null)
        {
            _consumed = true;
        }

        return row;
    }

    /// <summary>Single-pass enumeration over the remaining rows.</summary>
    public IEnumerator<Row> GetEnumerator()
    {
        if (_enumerationStarted || _consumed)
        {
            throw new InvalidOperationException(
                "A query cursor can only be enumerated once. " +
                "Execute the statement again for a second pass.");
        }

        _enumerationStarted = true;
        return new Enumerator(this);
    }

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    /// <summary>
    /// Releases the native cursor. Called automatically when the owning
    /// <see cref="Geodatabase"/> or <see cref="Table"/> is disposed. Safe to
    /// call more than once.
    /// </summary>
    public void Dispose()
    {
        CloseNative();
        _owner.UnregisterCursor(this);
        GC.SuppressFinalize(this);
    }

    ~QueryCursor()
    {
        try
        {
            CloseNative();
        }
        catch
        {
            // Finalizer must never throw.
        }
    }

    /// <summary>Called by the owner when the geodatabase or table is closed.</summary>
    internal void CloseFromOwner()
    {
        _aborted = true;
        CloseNative();
    }

    private void CloseNative()
    {
        if (_handle == IntPtr.Zero)
        {
            return;
        }

        IntPtr handle = _handle;
        _handle = IntPtr.Zero;
        _ended = true;
        NativeMethods.CursorClose(handle);
    }

    private Row? ReadCore()
    {
        if (_aborted)
        {
            throw new EsriGdbException(
                $"The owning {_owner.OwnerKind} was closed while the cursor was still reading " +
                "the result set.",
                -1);
        }

        if (_handle == IntPtr.Zero || _ended)
        {
            return null;
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        NativeMethods.EgdbRow nativeRow = default;

        int status;
        try
        {
            status = NativeMethods.CursorNext(_handle, out nativeRow, errorBuffer, errorBuffer.Length);
        }
        catch (DllNotFoundException e)
        {
            throw new EsriGdbException(
                $"The native library '{NativeMethods.LibraryName}' could not be loaded. " +
                NativeMethods.LoadFailureHint, -1, e);
        }
        catch (ObjectDisposedException)
        {
            _ended = true;
            return null;
        }

        if (status == NativeMethods.CursorEnd)
        {
            _ended = true;
            return null;
        }

        if (status != NativeMethods.CursorRow)
        {
            NativeMethods.ThrowOnError(status, errorBuffer, "Reading the next row");
        }

        try
        {
            return CursorMarshal.ReadRow(nativeRow, _fields, _nameToIndex);
        }
        finally
        {
            NativeMethods.FreeRow(ref nativeRow);
        }
    }

    private sealed class Enumerator : IEnumerator<Row>
    {
        private readonly QueryCursor _cursor;

        internal Enumerator(QueryCursor cursor) => _cursor = cursor;

        public Row Current { get; private set; } = null!;

        object IEnumerator.Current => Current;

        public bool MoveNext()
        {
            Row? row = _cursor.ReadCore();
            if (row is null)
            {
                _cursor._consumed = true;
                return false;
            }

            Current = row;
            return true;
        }

        public void Reset() => throw new NotSupportedException("A query cursor cannot be reset.");

        public void Dispose()
        {
            // Nothing to release: the cursor itself owns the native resources.
            // (The enumeration stays single pass - _enumerationStarted is never cleared.)
        }
    }
}

/// <summary>Marshals native cursor structures into managed results.</summary>
internal static class CursorMarshal
{
    internal static Field[] ReadFields(IntPtr cursor)
    {
        int fieldCount = Math.Max(NativeMethods.CursorFieldCount(cursor), 0);
        var fields = new Field[fieldCount];

        for (int i = 0; i < fieldCount; i++)
        {
            IntPtr namePointer = NativeMethods.CursorFieldName(cursor, i);
            string name = (namePointer == IntPtr.Zero ? null : System.Runtime.InteropServices.Marshal.PtrToStringUTF8(namePointer))
                ?? $"field{i}";

            int rawType = NativeMethods.CursorFieldType(cursor, i);
            FieldType type = rawType < 0 ? FieldType.String : (FieldType)rawType;

            int length = NativeMethods.CursorFieldLength(cursor, i);
            bool nullable = NativeMethods.CursorFieldNullable(cursor, i) != 0;

            fields[i] = new Field(i, name, type, length, nullable);
        }

        return fields;
    }

    internal static Dictionary<string, int> BuildNameMap(Field[] fields)
    {
        var map = new Dictionary<string, int>(fields.Length, StringComparer.Ordinal);
        for (int i = 0; i < fields.Length; i++)
        {
            map.TryAdd(fields[i].Name, i);
        }

        return map;
    }

    internal static Row ReadRow(NativeMethods.EgdbRow nativeRow, Field[] fields, Dictionary<string, int> nameToIndex)
    {
        var values = new object?[fields.Length];

        for (int i = 0; i < fields.Length; i++)
        {
            values[i] = ValueConverter.Parse(fields[i].Type, ReadValue(nativeRow, i));
        }

        return new Row(fields, values, nameToIndex);
    }

    private static string? ReadValue(NativeMethods.EgdbRow nativeRow, int index)
    {
        if (nativeRow.Values == IntPtr.Zero || index >= nativeRow.FieldCount)
        {
            return null;
        }

        IntPtr item = System.Runtime.InteropServices.Marshal.ReadIntPtr(nativeRow.Values, index * IntPtr.Size);
        return item == IntPtr.Zero ? null : System.Runtime.InteropServices.Marshal.PtrToStringUTF8(item);
    }
}
