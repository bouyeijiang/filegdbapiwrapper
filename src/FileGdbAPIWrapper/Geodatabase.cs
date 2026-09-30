using System.Runtime.InteropServices;

namespace EsriGdb;

/// <summary>
/// An open file geodatabase. Entry point of the wrapper:
/// <code>
/// using var gdb = Geodatabase.Open(@"./MyData.gdb");
/// QueryResult result = gdb.ExecuteSql("SELECT CITY_NAME, POP1990 FROM Cities");
/// </code>
/// </summary>
public sealed class Geodatabase : IDisposable, ICursorOwner
{
    private long _handle;
    private bool _disposed;

    // Cursors still streaming from this geodatabase; closed together with it.
    private readonly object _cursorsSync = new();
    private readonly HashSet<QueryCursor> _cursors = new();

    // Tables opened (or created) through this geodatabase.
    private readonly HashSet<Table> _tables = new();

    private Geodatabase(long handle) => _handle = handle;

    /// <summary>Handle of the open geodatabase (0 when closed).</summary>
    internal long Handle => _handle;

    /// <summary>Version string of the underlying native bridge.</summary>
    public static string NativeVersion
    {
        get
        {
            try
            {
                return NativeMethods.Version();
            }
            catch (DllNotFoundException)
            {
                return string.Empty;
            }
        }
    }

    /// <summary>True while the geodatabase is open.</summary>
    public bool IsOpen => !_disposed && _handle != 0;

    /// <summary>
    /// Opens the file geodatabase located at <paramref name="path"/> (a <c>.gdb</c> folder).
    /// </summary>
    /// <exception cref="EsriGdbException">The geodatabase could not be opened.</exception>
    public static Geodatabase Open(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            throw new ArgumentException("A geodatabase path is required.", nameof(path));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode;
        long handle;

        try
        {
            errorCode = NativeMethods.Open(path, out handle, errorBuffer, errorBuffer.Length);
        }
        catch (DllNotFoundException e)
        {
            throw new EsriGdbException(
                $"The native library '{NativeMethods.LibraryName}' could not be loaded. " +
                $"{NativeMethods.LoadFailureHint} Loader said: {e.Message}",
                -1,
                e);
        }

        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Opening geodatabase '{path}'");
        return new Geodatabase(handle);
    }

    /// <summary>
    /// Executes a SQL statement against the geodatabase and returns the full result set.
    /// Statements that do not produce rows (UPDATE/INSERT/DELETE/DDL) return an empty result.
    /// </summary>
    /// <param name="sql">SQL statement, e.g. <c>SELECT ... FROM Cities WHERE ...</c></param>
    /// <exception cref="EsriGdbException">The statement failed or the geodatabase is closed.</exception>
    public QueryResult ExecuteSql(string sql)
    {
        EnsureOpen();

        if (string.IsNullOrWhiteSpace(sql))
        {
            throw new ArgumentException("A SQL statement is required.", nameof(sql));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        NativeMethods.EgdbTable table = default;

        try
        {
            int errorCode = NativeMethods.ExecuteSql(_handle, sql, out table, errorBuffer, errorBuffer.Length);
            NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Executing SQL '{sql}'");
            return Materialize(in table);
        }
        finally
        {
            NativeMethods.FreeTable(ref table);
        }
    }

    /// <summary>
    /// Executes a SQL statement and returns a <strong>streaming</strong> cursor over its
    /// result set: rows are pulled from the native layer on demand, so memory use does
    /// not grow with the number of rows.
    /// </summary>
    /// <param name="sql">SQL statement, e.g. <c>SELECT ... FROM HugeTable</c></param>
    /// <remarks>
    /// Dispose the cursor when done (or close/dispose the geodatabase - it closes its
    /// remaining cursors). The geodatabase must stay open while the cursor is in use.
    /// </remarks>
    /// <exception cref="EsriGdbException">The statement failed or the geodatabase is closed.</exception>
    public QueryCursor ExecuteSqlCursor(string sql)
    {
        EnsureOpen();

        if (string.IsNullOrWhiteSpace(sql))
        {
            throw new ArgumentException("A SQL statement is required.", nameof(sql));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        IntPtr cursor;

        try
        {
            int errorCode = NativeMethods.ExecuteSqlCursor(_handle, sql, out cursor, errorBuffer, errorBuffer.Length);
            NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Executing SQL '{sql}'");
        }
        catch (DllNotFoundException e)
        {
            throw new EsriGdbException(
                $"The native library '{NativeMethods.LibraryName}' could not be loaded. " +
                NativeMethods.LoadFailureHint,
                -1,
                e);
        }

        return QueryCursor.Attach(
            this,
            cursor,
            () => IsOpen,
            "The geodatabase was closed while the query was being set up.");
    }

    /// <summary>Closes every cursor still streaming from this geodatabase.</summary>
    private void CloseCursors()
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
    // ICursorOwner: a geodatabase owns SQL cursors plus the cursors of its
    // tables (a table's cursors are unregistered by the table itself).
    // ------------------------------------------------------------------

    object ICursorOwner.CursorSync => _cursorsSync;

    string ICursorOwner.OwnerKind => "geodatabase";

    void ICursorOwner.AddCursor(QueryCursor cursor) => _cursors.Add(cursor);

    void ICursorOwner.UnregisterCursor(QueryCursor cursor)
    {
        lock (_cursorsSync)
        {
            _cursors.Remove(cursor);
        }
    }

    void ICursorOwner.CloseAllCursors() => CloseCursors();

    /// <summary>Closes every table still open on this geodatabase.</summary>
    private void CloseTables()
    {
        Table[] pending;
        lock (_cursorsSync)
        {
            if (_tables.Count == 0)
            {
                return;
            }

            pending = _tables.ToArray();
            _tables.Clear();
        }

        foreach (Table table in pending)
        {
            table.CloseFromOwner();
        }
    }

    /// <summary>Removes a table from the registry (called when the table is closed).</summary>
    internal void UnregisterTable(Table table)
    {
        lock (_cursorsSync)
        {
            _tables.Remove(table);
        }
    }

    // ------------------------------------------------------------------
    // Geodatabase management
    // ------------------------------------------------------------------
    /// <summary>
    /// Creates a new file geodatabase (a <c>.gdb</c> folder) and returns it open.
    /// </summary>
    /// <exception cref="EsriGdbException">The geodatabase could not be created.</exception>
    public static Geodatabase Create(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            throw new ArgumentException("A geodatabase path is required.", nameof(path));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.CreateGeodatabase(path, out long handle, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Creating geodatabase '{path}'");
        return new Geodatabase(handle);
    }

    /// <summary>
    /// Deletes the file geodatabase at <paramref name="path"/>. It must not be open.
    /// </summary>
    /// <exception cref="EsriGdbException">The geodatabase could not be deleted.</exception>
    public static void Delete(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            throw new ArgumentException("A geodatabase path is required.", nameof(path));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.DeleteGeodatabase(path, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Deleting geodatabase '{path}'");
    }

    /// <summary>
    /// Compacts the geodatabase, reclaiming space after bulk updates. Recommended
    /// after large insert/update/delete sessions.
    /// </summary>
    /// <exception cref="EsriGdbException">The geodatabase could not be compacted.</exception>
    public void Compact()
    {
        EnsureOpen();

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.CompactDatabase(_handle, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, "Compacting the geodatabase");
    }

    // ------------------------------------------------------------------
    // Catalog browsing
    // ------------------------------------------------------------------

    /// <summary>All dataset types of the geodatabase (e.g. "Feature Class", "Table").</summary>
    /// <exception cref="EsriGdbException">The geodatabase is closed or the query failed.</exception>
    public IReadOnlyList<string> GetDatasetTypes()
    {
        EnsureOpen();

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        NativeMethods.EgdbStringArray array = default;

        try
        {
            int errorCode = NativeMethods.GetDatasetTypes(_handle, out array, errorBuffer, errorBuffer.Length);
            NativeMethods.ThrowOnError(errorCode, errorBuffer, "Listing the dataset types");
            return NativeMethods.ReadStringArray(array);
        }
        finally
        {
            NativeMethods.FreeStringArray(ref array);
        }
    }

    /// <summary>
    /// Lists the datasets contained in a dataset: pass no arguments for the
    /// datasets at the root of the geodatabase, or filter by type
    /// (e.g. <c>"Feature Class"</c>, <c>"Table"</c>).
    /// </summary>
    /// <param name="parentPath">Path of the container dataset, e.g. <c>@"\CitiesFD"</c>.</param>
    /// <param name="datasetType">Dataset type filter; <c>null</c> for all types.</param>
    /// <exception cref="EsriGdbException">The geodatabase is closed or the query failed.</exception>
    public IReadOnlyList<string> GetChildDatasets(string? parentPath = null, string? datasetType = null)
    {
        EnsureOpen();

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        NativeMethods.EgdbStringArray array = default;

        try
        {
            int errorCode = NativeMethods.GetChildDatasets(
                _handle,
                parentPath ?? string.Empty,
                datasetType ?? string.Empty,
                out array,
                errorBuffer,
                errorBuffer.Length);
            NativeMethods.ThrowOnError(errorCode, errorBuffer, "Listing the child datasets");
            return NativeMethods.ReadStringArray(array);
        }
        finally
        {
            NativeMethods.FreeStringArray(ref array);
        }
    }

    /// <summary>
    /// Returns the name to use for a dataset inside SQL statements
    /// (the path <c>@"\Cities"</c> resolves to the query name <c>Cities</c>).
    /// </summary>
    /// <exception cref="EsriGdbException">The geodatabase is closed or the path is unknown.</exception>
    public string GetQueryName(string path)
    {
        EnsureOpen();
        RequirePath(path, nameof(path));

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.GetQueryName(_handle, path, out IntPtr name, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Resolving the query name of '{path}'");
        return NativeMethods.TakeString(name);
    }

    /// <summary>The XML data element definition of a dataset.</summary>
    /// <param name="path">Dataset path, e.g. <c>@"\Cities"</c>.</param>
    /// <param name="datasetType">Dataset type, e.g. <c>"Feature Class"</c>.</param>
    /// <exception cref="EsriGdbException">The dataset was not found.</exception>
    public string GetDatasetDefinition(string path, string datasetType)
    {
        EnsureOpen();
        RequirePath(path, nameof(path));

        if (string.IsNullOrWhiteSpace(datasetType))
        {
            throw new ArgumentException("A dataset type is required.", nameof(datasetType));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.GetDatasetDefinition(
            _handle, path, datasetType, out IntPtr xml, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(
            errorCode, errorBuffer, $"Reading the definition of '{path}' ({datasetType})");
        return NativeMethods.TakeString(xml);
    }

    // ------------------------------------------------------------------
    // Domains
    // ------------------------------------------------------------------

    /// <summary>Names of all domains defined in the geodatabase.</summary>
    /// <exception cref="EsriGdbException">The geodatabase is closed or the query failed.</exception>
    public IReadOnlyList<string> GetDomains()
    {
        EnsureOpen();

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        NativeMethods.EgdbStringArray array = default;

        try
        {
            int errorCode = NativeMethods.GetDomains(_handle, out array, errorBuffer, errorBuffer.Length);
            NativeMethods.ThrowOnError(errorCode, errorBuffer, "Listing the domains");
            return NativeMethods.ReadStringArray(array);
        }
        finally
        {
            NativeMethods.FreeStringArray(ref array);
        }
    }

    /// <summary>XML definition of a domain (coded value domain or range domain).</summary>
    /// <exception cref="EsriGdbException">The domain was not found.</exception>
    public string GetDomainDefinition(string domainName)
    {
        EnsureOpen();
        if (string.IsNullOrWhiteSpace(domainName))
        {
            throw new ArgumentException("A domain name is required.", nameof(domainName));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.GetDomainDefinition(
            _handle, domainName, out IntPtr xml, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Reading the definition of domain '{domainName}'");
        return NativeMethods.TakeString(xml);
    }

    /// <summary>
    /// Creates a domain from an XML definition. The XML returned by
    /// <see cref="GetDomainDefinition"/> can be passed back unchanged.
    /// </summary>
    /// <exception cref="EsriGdbException">The definition is invalid or the name is taken.</exception>
    public void CreateDomain(string domainDefinitionXml)
    {
        EnsureOpen();
        if (string.IsNullOrWhiteSpace(domainDefinitionXml))
        {
            throw new ArgumentException("A domain definition is required.", nameof(domainDefinitionXml));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.CreateDomain(_handle, domainDefinitionXml, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, "Creating the domain");
    }

    /// <summary>Deletes a domain by name.</summary>
    /// <exception cref="EsriGdbException">The domain does not exist.</exception>
    public void DeleteDomain(string domainName)
    {
        EnsureOpen();
        if (string.IsNullOrWhiteSpace(domainName))
        {
            throw new ArgumentException("A domain name is required.", nameof(domainName));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.DeleteDomain(_handle, domainName, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Deleting domain '{domainName}'");
    }

    // ------------------------------------------------------------------
    // Tables
    // ------------------------------------------------------------------

    /// <summary>
    /// Opens a table or feature class for schema inspection, searching and editing.
    /// Dispose the returned table (or the geodatabase) when finished.
    /// </summary>
    /// <param name="path">Table path, e.g. <c>"Cities"</c> or <c>@"\Cities"</c>.</param>
    /// <exception cref="EsriGdbException">The table does not exist.</exception>
    public Table OpenTable(string path)
    {
        EnsureOpen();
        RequirePath(path, nameof(path));

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.OpenTable(_handle, path, out IntPtr handle, errorBuffer, errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, $"Opening table '{path}'");

        return RegisterTable(handle, path);
    }

    /// <summary>
    /// Creates a table or feature class from an XML data element definition
    /// (see <c>samples/XMLsamples/Table.xml</c> for the expected shape) and
    /// returns it open.
    /// </summary>
    /// <param name="tableDefinitionXml">XML data element definition.</param>
    /// <param name="parentPath">Feature dataset to create it in, or <c>null</c> for the root.</param>
    /// <exception cref="EsriGdbException">The definition is invalid or the dataset exists.</exception>
    public Table CreateTable(string tableDefinitionXml, string? parentPath = null)
    {
        EnsureOpen();
        if (string.IsNullOrWhiteSpace(tableDefinitionXml))
        {
            throw new ArgumentException("A table definition is required.", nameof(tableDefinitionXml));
        }

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.CreateTable(
            _handle,
            tableDefinitionXml,
            parentPath ?? string.Empty,
            out IntPtr handle,
            errorBuffer,
            errorBuffer.Length);
        NativeMethods.ThrowOnError(errorCode, errorBuffer, "Creating the table");

        string created = ExtractXmlValue(tableDefinitionXml, "CatalogPath");
        if (string.IsNullOrEmpty(created))
        {
            created = ExtractXmlValue(tableDefinitionXml, "Name");
        }

        return RegisterTable(handle, created);
    }

    /// <summary>Registers a table opened or created through this geodatabase.</summary>
    private Table RegisterTable(IntPtr handle, string path)
    {
        var table = new Table(this, _handle, handle, path);

        lock (_cursorsSync)
        {
            if (!IsOpen)
            {
                throw new EsriGdbException("The geodatabase was closed.", -1);
            }

            _tables.Add(table);
        }

        return table;
    }

    /// <summary>Pulls the CatalogPath (or Name) out of a data element definition.</summary>
    private static string ExtractXmlValue(string xml, string element)
    {
        string open = $"<{element}>";
        int start = xml.IndexOf(open, StringComparison.Ordinal);
        if (start < 0)
        {
            return string.Empty;
        }

        start += open.Length;
        int end = xml.IndexOf($"</{element}>", start, StringComparison.Ordinal);
        return end < 0 ? string.Empty : xml[start..end].Trim();
    }

    private static void RequirePath(string path, string parameterName)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            throw new ArgumentException("A dataset path is required.", parameterName);
        }
    }

    /// <summary>Closes the geodatabase. Safe to call more than once.</summary>
    public void Close()
    {
        if (_disposed || _handle == 0)
        {
            return;
        }

        // Order matters: invalidate the managed objects before the native layer
        // releases the query state they point at.
        CloseTables();
        CloseCursors();

        byte[] errorBuffer = NativeMethods.NewErrorBuffer();
        int errorCode = NativeMethods.Close(_handle, errorBuffer, errorBuffer.Length);

        _handle = 0;
        _disposed = true;

        NativeMethods.ThrowOnError(errorCode, errorBuffer, "Closing the geodatabase");
    }

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

    ~Geodatabase()
    {
        if (!_disposed && _handle != 0)
        {
            try
            {
                CloseTables();
                CloseCursors();
                NativeMethods.Close(_handle, NativeMethods.NewErrorBuffer(), NativeMethods.ErrorBufferSize);
            }
            catch
            {
                // Finalizer must never throw.
            }

            _handle = 0;
        }
    }

    private void EnsureOpen()
    {
        if (!IsOpen)
        {
            throw new EsriGdbException("The geodatabase is not open.", -1);
        }
    }

    /// <summary>Copies the native table into managed objects.</summary>
    private static QueryResult Materialize(in NativeMethods.EgdbTable table)
    {
        int fieldCount = Math.Max(table.FieldCount, 0);
        int rowCount = Math.Max(table.RowCount, 0);

        var fields = new Field[fieldCount];
        var nameToIndex = new Dictionary<string, int>(fieldCount, StringComparer.Ordinal);

        for (int i = 0; i < fieldCount; i++)
        {
            string name = ReadString(table.FieldNames, i) ?? $"field{i}";

            FieldType type = table.FieldTypes == IntPtr.Zero
                ? FieldType.String
                : (FieldType)Marshal.ReadInt32(table.FieldTypes, i * sizeof(int));

            fields[i] = new Field(i, name, type);
            nameToIndex.TryAdd(name, i);
        }

        var rows = new Row[rowCount];

        for (int r = 0; r < rowCount; r++)
        {
            var values = new object?[fieldCount];

            for (int c = 0; c < fieldCount; c++)
            {
                string? text = ReadString(table.Values, r * fieldCount + c);
                values[c] = ValueConverter.Parse(fields[c].Type, text);
            }

            rows[r] = new Row(fields, values, nameToIndex);
        }

        return new QueryResult(fields, rows);
    }

    /// <summary>Reads the UTF-8 string pointer at <paramref name="index"/> of a <c>char**</c>.</summary>
    private static string? ReadString(IntPtr pointerArray, int index)
    {
        if (pointerArray == IntPtr.Zero)
        {
            return null;
        }

        IntPtr item = Marshal.ReadIntPtr(pointerArray, index * IntPtr.Size);
        return item == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(item);
    }
}
