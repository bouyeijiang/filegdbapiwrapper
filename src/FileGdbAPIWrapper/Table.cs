using System.Runtime.InteropServices;

namespace EsriGdb;

/// <summary>
/// An open table or feature class of a <see cref="Geodatabase"/>. Gives access to
/// the schema, to streaming searches and to row editing:
///
/// <code>
/// using Table cities = geodatabase.OpenTable("Cities");
/// foreach (Field field in cities.Fields)
///     Console.WriteLine(field);
///
/// using QueryCursor cursor = cities.Search(whereClause: "POP1990 > 100000");
/// while (cursor.ReadNext() is Row row)
///     Console.WriteLine(row["CITY_NAME"]);
/// </code>
///
/// Dispose the table when finished (or close/dispose the owning geodatabase -
/// both close the cursors still streaming from this table).
/// </summary>
public sealed class Table : IDisposable, ICursorOwner
{
    private readonly Geodatabase _owner;
    private readonly long _gdbHandle;
    private readonly IntPtr _handle;
    private readonly string _path;
    private bool _disposed;

    private readonly object _cursorsSync = new();
    private readonly HashSet<QueryCursor> _cursors = new();
    private Field[]? _fields;

    internal Table(Geodatabase owner, long gdbHandle, IntPtr handle, string path)
    {
        _owner = owner;
        _gdbHandle = gdbHandle;
        _handle = handle;
        _path = path;
    }

    /// <summary>Path of the table inside the geodatabase (may be empty for created tables).</summary>
    public string Path => _path;

    /// <summary>True while the table is open.</summary>
    public bool IsOpen => !_disposed && _handle != IntPtr.Zero;

    /// <summary>
    /// Schema of the table. Read once when first accessed and refreshed after
    /// <see cref="AddField"/> / <see cref="DeleteField"/>.
    /// </summary>
    public IReadOnlyList<Field> Fields
    {
        get
        {
            EnsureOpen();
            return _fields ??= ReadFields();
        }
    }

    /// <summary>The field with the given name (case insensitive), or <c>null</c>.</summary>
    public Field? FindField(string name)
    {
        foreach (Field field in Fields)
        {
            if (string.Equals(field.Name, name, StringComparison.OrdinalIgnoreCase))
            {
                return field;
            }
        }

        return null;
    }

    /// <summary>The XML data element definition (schema) of the table.</summary>
    /// <exception cref="EsriGdbException">The table is closed or the schema could not be read.</exception>
    public string GetDefinition()
    {
        EnsureOpen();

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.TableGetDefinition(
            _handle, out IntPtr xml, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, "Reading the table definition");
        return NativeMethods.TakeString(xml);
    }

    // ------------------------------------------------------------------
    // Searching
    // ------------------------------------------------------------------

    /// <summary>
    /// Runs an attribute query (optionally restricted to a spatial envelope) and
    /// returns a <strong>streaming</strong> cursor over the matching rows.
    /// </summary>
    /// <param name="subfields">Fields to return; <c>null</c> or empty for all of them.</param>
    /// <param name="whereClause">Predicate, e.g. <c>POP1990 &gt; 100000</c>; <c>null</c> for no filter.</param>
    /// <param name="envelope">Optional spatial filter in the feature class coordinate system.</param>
    /// <param name="recycling">
    /// <c>true</c> (the default) recycles memory while reading and is what you want for
    /// pure reads. Use <c>false</c> only when rows will be modified through the native
    /// API - the managed edit methods (<see cref="Update"/>/<see cref="Delete"/>) do that
    /// internally already.
    /// </param>
    /// <remarks>Dispose the cursor when done; it is closed automatically with the table.</remarks>
    /// <exception cref="EsriGdbException">The query failed or the table is closed.</exception>
    public QueryCursor Search(
        string? subfields = null,
        string? whereClause = null,
        Envelope? envelope = null,
        bool recycling = true)
    {
        EnsureOpen();

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.TableSearch(
            _handle,
            subfields ?? string.Empty,
            whereClause ?? string.Empty,
            NativeMethods.EgdbEnvelope.From(envelope),
            recycling ? 1 : 0,
            out IntPtr cursor,
            errorBuffer,
            errorBuffer.Length);

        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Searching table '{DisplayName}'");

        return QueryCursor.Attach(
            this,
            cursor,
            () => IsOpen,
            $"Table '{DisplayName}' was closed while the search was being set up.");
    }

    // ------------------------------------------------------------------
    // Editing
    // ------------------------------------------------------------------

    /// <summary>
    /// Inserts a row and returns its new <c>OBJECTID</c>.
    /// Fields that are not listed keep their default value; a <c>null</c> value
    /// writes SQL NULL, a <c>byte[]</c> is stored as Blob/geometry bytes.
    /// </summary>
    /// <exception cref="EsriGdbException">A value was rejected or the insert failed.</exception>
    public int InsertRow(IReadOnlyDictionary<string, object?> values)
    {
        EnsureOpen();
        ArgumentNullException.ThrowIfNull(values);

        (IntPtr[] names, IntPtr[] cells, List<IntPtr> allocations) = BuildValueArrays(values);

        try
        {
            byte[] errorBuffer = NativeMethods.NewErrorBuffer();
            int errorCode = NativeMethods.TableInsert(
                _handle, names, cells, names.Length, out int newOid, errorBuffer, errorBuffer.Length);
            NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Inserting a row into '{DisplayName}'");
            return newOid;
        }
        finally
        {
            FreeAllocations(allocations);
        }
    }

    /// <inheritdoc cref="InsertRow(IReadOnlyDictionary{string, object?})"/>
    public int InsertRow(params (string Name, object? Value)[] values) =>
        InsertRow(ToDictionary(values));

    /// <summary>
    /// Updates every row matching <paramref name="whereClause"/> / <paramref name="envelope"/>
    /// and returns the number of rows that were changed. Without a filter every row
    /// of the table is updated.
    /// </summary>
    /// <exception cref="EsriGdbException">A value was rejected or the update failed.</exception>
    public int Update(
        IReadOnlyDictionary<string, object?> values,
        string? whereClause = null,
        Envelope? envelope = null)
    {
        EnsureOpen();
        ArgumentNullException.ThrowIfNull(values);
        if (values.Count == 0)
        {
            throw new ArgumentException("At least one field value is required.", nameof(values));
        }

        (IntPtr[] names, IntPtr[] cells, List<IntPtr> allocations) = BuildValueArrays(values);

        try
        {
            byte[] errorBuffer = NativeMethods.NewErrorBuffer();
            int errorCode = NativeMethods.TableUpdate(
                _handle,
                whereClause ?? string.Empty,
                NativeMethods.EgdbEnvelope.From(envelope),
                names,
                cells,
                names.Length,
                out int affected,
                errorBuffer,
                errorBuffer.Length);

            NativeMethods.ThrowOnError(
                errorCode, errorBuffer, $"Updating rows of '{DisplayName}'" + FilterSuffix(whereClause, envelope));
            return affected;
        }
        finally
        {
            FreeAllocations(allocations);
        }
    }

    /// <inheritdoc cref="Update(IReadOnlyDictionary{string, object?}, string, Envelope?)"/>
    public int Update(
        (string Name, object? Value)[] values,
        string? whereClause = null,
        Envelope? envelope = null) =>
        Update(ToDictionary(values), whereClause, envelope);

    /// <summary>
    /// Deletes every row matching <paramref name="whereClause"/> / <paramref name="envelope"/>
    /// and returns the number of rows that were removed. Without a filter the whole
    /// table is emptied - make sure that is what you want.
    /// </summary>
    /// <exception cref="EsriGdbException">The delete failed.</exception>
    public int Delete(string? whereClause = null, Envelope? envelope = null)
    {
        EnsureOpen();

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.TableDelete(
            _handle,
            whereClause ?? string.Empty,
            NativeMethods.EgdbEnvelope.From(envelope),
            out int affected,
            errorBuffer,
            errorBuffer.Length);

        NativeMethods.ThrowOnError(
            errorCode, errorBuffer, $"Deleting rows of '{DisplayName}'" + FilterSuffix(whereClause, envelope));
        return affected;
    }

    // ------------------------------------------------------------------
    // Schema
    // ------------------------------------------------------------------

    /// <summary>
    /// Adds a field from an XML field definition. The bridge repairs missing
    /// namespace declarations, so a fragment such as
    /// <c>&lt;Field&gt;&lt;Name&gt;NOTES&lt;/Name&gt;...&lt;/Field&gt;</c> is accepted as well.
    /// </summary>
    /// <exception cref="EsriGdbException">The field already exists or the XML is invalid.</exception>
    public void AddField(string fieldDefinitionXml)
    {
        EnsureOpen();
        if (string.IsNullOrWhiteSpace(fieldDefinitionXml))
        {
            throw new ArgumentException("A field definition is required.", nameof(fieldDefinitionXml));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.TableAddField(
            _handle, fieldDefinitionXml, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Adding a field to '{DisplayName}'");
        _fields = null;   // schema changed
    }

    /// <summary>Deletes a field by name.</summary>
    /// <exception cref="EsriGdbException">The field does not exist.</exception>
    public void DeleteField(string fieldName)
    {
        EnsureOpen();
        if (string.IsNullOrWhiteSpace(fieldName))
        {
            throw new ArgumentException("A field name is required.", nameof(fieldName));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.TableDeleteField(
            _handle, fieldName, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Deleting field '{fieldName}'");
        _fields = null;
    }

    /// <summary>Adds an index from an XML index definition.</summary>
    /// <exception cref="EsriGdbException">The index already exists or the XML is invalid.</exception>
    public void AddIndex(string indexDefinitionXml)
    {
        EnsureOpen();
        if (string.IsNullOrWhiteSpace(indexDefinitionXml))
        {
            throw new ArgumentException("An index definition is required.", nameof(indexDefinitionXml));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.TableAddIndex(
            _handle, indexDefinitionXml, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Adding an index to '{DisplayName}'");
    }

    // ------------------------------------------------------------------
    // Lifetime
    // ------------------------------------------------------------------

    /// <summary>
    /// Closes the table and every cursor still streaming from it. Safe to call
    /// more than once.
    /// </summary>
    /// <exception cref="EsriGdbException">The native layer rejected the close.</exception>
    public void Close()
    {
        if (_disposed)
        {
            return;
        }

        // Invalidate the managed cursors before the native layer releases the
        // query state they point at.
        CloseOwnedCursors();
        _disposed = true;
        _owner.UnregisterTable(this);

        if (!_owner.IsOpen)
        {
            return;   // closing the geodatabase already released the table natively
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.CloseTable(_gdbHandle, _handle, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Closing table '{DisplayName}'");
    }

    /// <summary>Closes the table. Safe to call more than once; never throws.</summary>
    public void Dispose()
    {
        try
        {
            Close();
        }
        catch (EsriGdbException)
        {
            // Dispose must not throw; the handle has been released either way.
        }

        GC.SuppressFinalize(this);
    }

    ~Table()
    {
        try
        {
            if (!_disposed)
            {
                CloseOwnedCursors();
                _disposed = true;

                if (_owner.IsOpen)
                {
                    NativeMethods.CloseTable(
                        _gdbHandle, _handle, NativeMethods.NewErrorBuffer(), NativeMethods.ErrorBufferSize);
                }

                _owner.UnregisterTable(this);
            }
        }
        catch
        {
            // Finalizer must never throw.
        }
    }

    /// <summary>Called by the owner when the geodatabase itself is closed.</summary>
    internal void CloseFromOwner()
    {
        CloseOwnedCursors();
        _disposed = true;
    }

    // ------------------------------------------------------------------
    // ICursorOwner
    // ------------------------------------------------------------------

    object ICursorOwner.CursorSync => _cursorsSync;

    string ICursorOwner.OwnerKind => "table";

    void ICursorOwner.AddCursor(QueryCursor cursor) => _cursors.Add(cursor);

    void ICursorOwner.UnregisterCursor(QueryCursor cursor)
    {
        lock (_cursorsSync)
        {
            _cursors.Remove(cursor);
        }
    }

    void ICursorOwner.CloseAllCursors() => CloseOwnedCursors();

    private void CloseOwnedCursors()
    {
        QueryCursor[] pending;
        lock (_cursorsSync)
        {
            if (_cursors.Count == 0)
            {
                return;
            }

            pending = _cursors.ToArray();
            _cursors.Clear();
        }

        foreach (QueryCursor cursor in pending)
        {
            cursor.CloseFromOwner();
        }
    }

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    private string DisplayName => string.IsNullOrEmpty(_path) ? "table" : _path;

    private void EnsureOpen()
    {
        if (!IsOpen)
        {
            throw new EsriGdbException("The table is not open.", -1);
        }
    }

    private Field[] ReadFields()
    {
        int count = Math.Max(NativeMethods.TableFieldCount(_handle), 0);
        var fields = new Field[count];

        for (int i = 0; i < count; i++)
        {
            IntPtr namePointer = NativeMethods.TableFieldName(_handle, i);
            string name = (namePointer == IntPtr.Zero
                ? null
                : Marshal.PtrToStringUTF8(namePointer)) ?? $"field{i}";

            int rawType = NativeMethods.TableFieldType(_handle, i);
            FieldType type = rawType < 0 ? FieldType.String : (FieldType)rawType;

            int length = NativeMethods.TableFieldLength(_handle, i);
            bool nullable = NativeMethods.TableFieldNullable(_handle, i) != 0;

            fields[i] = new Field(i, name, type, length, nullable);
        }

        return fields;
    }

    /// <summary>
    /// Converts managed values into the parallel UTF-8 string arrays the native
    /// insert/update entry points expect. Every allocation is returned so the
    /// caller can release it.
    /// </summary>
    private static (IntPtr[] Names, IntPtr[] Cells, List<IntPtr> Allocations) BuildValueArrays(
        IReadOnlyDictionary<string, object?> values)
    {
        var names = new IntPtr[values.Count];
        var cells = new IntPtr[values.Count];
        var allocations = new List<IntPtr>(values.Count * 2);

        int index = 0;
        foreach (KeyValuePair<string, object?> pair in values)
        {
            if (string.IsNullOrWhiteSpace(pair.Key))
            {
                throw new ArgumentException("Field names must not be empty.", nameof(values));
            }

            IntPtr name = Marshal.StringToCoTaskMemUTF8(pair.Key);
            allocations.Add(name);
            names[index] = name;

            string? text = ValueConverter.Format(pair.Value);
            if (text is not null)
            {
                IntPtr cell = Marshal.StringToCoTaskMemUTF8(text);
                allocations.Add(cell);
                cells[index] = cell;
            }
            // IntPtr.Zero means SQL NULL.

            index++;
        }

        return (names, cells, allocations);
    }

    private static void FreeAllocations(List<IntPtr> allocations)
    {
        foreach (IntPtr pointer in allocations)
        {
            Marshal.FreeCoTaskMem(pointer);
        }
    }

    private static Dictionary<string, object?> ToDictionary((string Name, object? Value)[] values)
    {
        ArgumentNullException.ThrowIfNull(values);

        var map = new Dictionary<string, object?>(values.Length, StringComparer.Ordinal);
        foreach ((string name, object? value) in values)
        {
            map[name] = value;
        }

        return map;
    }

    private static string FilterSuffix(string? whereClause, Envelope? envelope)
    {
        if (!string.IsNullOrWhiteSpace(whereClause))
        {
            return $" where {whereClause}";
        }

        return envelope is null ? string.Empty : $" in {envelope}";
    }
}
