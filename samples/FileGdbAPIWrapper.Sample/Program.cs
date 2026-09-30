using System.Globalization;
using EsriGdb;

// Usage: dotnet run --project samples/EsriGdbWrapper.Sample -- <path-to-gdb> [sql] [edit]
//
// Defaults to a query equivalent to the Esri "ExecutingSQL" sample.
// Pass "edit" anywhere in the argument list to also run the insert / update /
// delete / schema demo - it MODIFIES the geodatabase you point it at
// (everything is cleaned up again, but take a copy first).

string gdbPath = args.Length > 0 && !string.IsNullOrWhiteSpace(args[0])
    ? args[0]
    : "/home/bouyei/Projects/FileGDB_API-RHEL8-64gcc8/samples/data/ExecuteSQL.gdb";

string? sqlArgument = null;
bool editDemo = false;
for (int i = 1; i < args.Length; i++)
{
    if (args[i].Equals("edit", StringComparison.OrdinalIgnoreCase))
    {
        editDemo = true;
    }
    else if (!string.IsNullOrWhiteSpace(args[i]))
    {
        sqlArgument = args[i];
    }
}

string sql = sqlArgument
    ?? "SELECT CITY_NAME, POP1990 FROM Cities WHERE TYPE = 'city' AND OBJECTID < 10";

Console.WriteLine($"Esri FileGDB API wrapper demo (native bridge {Geodatabase.NativeVersion})");
Console.WriteLine($"Geodatabase: {gdbPath}");
Console.WriteLine($"SQL        : {sql}");
Console.WriteLine();

try
{
    using Geodatabase geodatabase = Geodatabase.Open(gdbPath);

    QueryResult result = geodatabase.ExecuteSql(sql);

    // Column metadata ------------------------------------------------------
    Console.WriteLine($"Rows: {result.Count}, Columns: {result.Fields.Count}");
    foreach (Field field in result.Fields)
    {
        Console.Write($"  {field.Name} [{field.Type}]");
    }
    Console.WriteLine();
    Console.WriteLine(new string('-', 70));

    // Rows -----------------------------------------------------------------
    foreach (Row row in result)
    {
        Console.WriteLine(string.Join(" | ", row.Select(FormatValue)));
    }

    // A second statement exercising all value kinds ------------------------
    if (args.Length < 2)
    {
        Console.WriteLine();
        Console.WriteLine("SELECT * FROM Cities WHERE TYPE = 'city' AND OBJECTID < 4");
        Console.WriteLine(new string('-', 70));

        QueryResult all = geodatabase.ExecuteSql(
            "SELECT * FROM Cities WHERE TYPE = 'city' AND OBJECTID < 4");

        foreach (Row row in all)
        {
            foreach (Field field in all.Fields)
            {
                Console.WriteLine($"  {field.Name,-14} = {FormatValue(row[field.Index])}");
            }
            Console.WriteLine();
        }
    }

    // Typed access demo ----------------------------------------------------
    QueryResult typed = geodatabase.ExecuteSql(
        "SELECT CITY_NAME, POP1990 FROM Cities WHERE TYPE = 'city' AND OBJECTID < 5");

    Console.WriteLine("Typed access:");
    foreach (Row row in typed)
    {
        Console.WriteLine(
            $"  {row.GetString("CITY_NAME"),-16} pop={row.GetInt32("POP1990")?.ToString(CultureInfo.InvariantCulture) ?? "null"}");
    }

    // Streaming cursor demo ------------------------------------------------
    // Rows are pulled from the native layer on demand - no full result set in memory.
    Console.WriteLine();
    Console.WriteLine("Streaming cursor (first 3 of all cities):");
    using (QueryCursor cursor = geodatabase.ExecuteSqlCursor(
               "SELECT CITY_NAME, POP1990 FROM Cities ORDER BY CITY_NAME"))
    {
        Console.WriteLine($"  columns: {string.Join(", ", cursor.Fields.Select(f => f.Name))}");

        int shown = 0;
        while (cursor.ReadNext() is Row row && shown < 3)
        {
            Console.WriteLine($"  {row.GetString("CITY_NAME"),-20} {row.GetInt32("POP1990")}");
            shown++;
        }
        // Disposing early releases the native query - no further rows are read.
    }

    Console.WriteLine();
    Console.WriteLine("Streaming the whole table with foreach:");
    long total = 0;
    long population = 0;
    using (QueryCursor cursor = geodatabase.ExecuteSqlCursor("SELECT POP1990 FROM Cities"))
    {
        foreach (Row row in cursor)
        {
            total++;
            population += row.GetInt32("POP1990") ?? 0;
        }
    }
    Console.WriteLine($"  streamed {total} rows, total population = {population}");

    // Non SELECT statements: no result set, cursor is immediately exhausted.
    using (QueryCursor cursor = geodatabase.ExecuteSqlCursor(
               "UPDATE CitiesNoGeo SET POP1990 = POP1990 WHERE OBJECTID < 0"))
    {
        Console.WriteLine();
        Console.WriteLine($"UPDATE produced {cursor.Fields.Count} columns, next = " +
                          (cursor.ReadNext() is null ? "end of result set" : "row"));
    }

    // ---------------------------------------------------------------------
    // Catalog browsing (schema discovery without opening anything)
    // ---------------------------------------------------------------------
    Console.WriteLine();
    Console.WriteLine("Catalog:");
    IReadOnlyList<string> datasetTypes = geodatabase.GetDatasetTypes();
    IReadOnlyList<string> featureClasses = geodatabase.GetChildDatasets(datasetType: "Feature Class");
    IReadOnlyList<string> tables = geodatabase.GetChildDatasets(datasetType: "Table");
    Console.WriteLine($"  dataset types   : {datasetTypes.Count} ({string.Join(", ", datasetTypes.Take(6))} ...)");
    Console.WriteLine($"  feature classes : {string.Join(", ", featureClasses.Take(6))}");
    Console.WriteLine($"  plain tables    : {string.Join(", ", tables.Take(6))}");

    string tableName = featureClasses.Count > 0 ? featureClasses[0]
        : tables.Count > 0 ? tables[0]
        : string.Empty;

    if (tableName.Length == 0)
    {
        Console.WriteLine("  (no datasets to inspect)");
        return;
    }

    Console.WriteLine();
    Console.WriteLine($"Schema of {tableName}:");
    using Table table = geodatabase.OpenTable(tableName);
    foreach (Field field in table.Fields)
    {
        string length = field.Length > 0 ? field.Length.ToString(CultureInfo.InvariantCulture) : "-";
        Console.WriteLine(
            $"  {field.Index,2} {field.Name,-16} {field.Type,-12} len={length,-5} nullable={field.IsNullable}");
    }

    Console.WriteLine();
    Console.WriteLine("Table.Search (streaming, no full result set in memory):");
    using (QueryCursor cursor = table.Search())
    {
        int shown = 0;
        while (cursor.ReadNext() is Row row && shown < 3)
        {
            Console.WriteLine("  " + string.Join(" | ", row.Select(FormatValue)));
            shown++;
        }
        if (shown == 0)
        {
            Console.WriteLine("  (table is empty)");
        }
    }

    // A spatial filter combined with an attribute filter. The envelope below
    // spans everything - replace it with your area of interest (xmin, ymin,
    // xmax, ymax in the coordinate system of the feature class).
    Console.WriteLine();
    Console.WriteLine("Table.Search with an envelope filter:");
    using (QueryCursor cursor = table.Search(
        subfields: null,
        whereClause: null,
        envelope: new Envelope(-1.0e12, -1.0e12, 1.0e12, 1.0e12)))
    {
        long matched = 0;
        while (cursor.ReadNext() is not null)
        {
            matched++;
        }
        Console.WriteLine($"  rows inside the envelope: {matched}");
    }

    // ---------------------------------------------------------------------
    // Editing (insert / update / delete / schema) - only on request, because
    // it writes to the geodatabase you passed in.
    // ---------------------------------------------------------------------
    if (editDemo)
    {
        Console.WriteLine();
        Console.WriteLine($"Editing demo on {table.Path} (rows are created and removed again):");

        Field? writable = table.Fields.FirstOrDefault(
            field => field.Type == FieldType.String && field.IsNullable
                     && !string.Equals(field.Name, "OBJECTID", StringComparison.OrdinalIgnoreCase));

        if (writable is null)
        {
            Console.WriteLine("  no nullable string field to write to - skipping");
        }
        else
        {
            int maxLength = writable.Length > 0 ? writable.Length : 10;
            string marker = ("S" + Guid.NewGuid().ToString("N"));
            marker = marker[..Math.Min(marker.Length, maxLength)];

            try
            {
                int oid = table.InsertRow(new Dictionary<string, object?> { [writable.Name] = marker });
                Console.WriteLine($"  insert  -> OBJECTID {oid}");

                using (QueryCursor cursor = table.Search(whereClause: $"OBJECTID = {oid}"))
                {
                    Row? row = cursor.ReadNext();
                    Console.WriteLine($"  read    -> {writable.Name} = {row?.GetString(writable.Name)}");
                }

                string updatedMarker = marker[..^1] + "9";
                int updated = table.Update(
                    new Dictionary<string, object?> { [writable.Name] = updatedMarker },
                    whereClause: $"OBJECTID = {oid}");
                Console.WriteLine($"  update  -> {updated} row(s)");

                int deleted = table.Delete($"OBJECTID = {oid}");
                Console.WriteLine($"  delete  -> {deleted} row(s)");
            }
            catch (EsriGdbException e)
            {
                Console.WriteLine($"  editing is not possible here: {e.Message}");
            }
        }

        // Schema change: add a field, read it back, remove it again.
        try
        {
            const string fieldName = "ESRIGDB_DEMO";
            if (table.FindField(fieldName) is null)
            {
                table.AddField(
                    $"<Field><Name>{fieldName}</Name><Type>esriFieldTypeString</Type>" +
                    "<IsNullable>true</IsNullable><Length>20</Length><Precision>0</Precision>" +
                    "<Scale>0</Scale><AliasName>Demo</AliasName><ModelName>DEMO</ModelName></Field>");
            }

            Console.WriteLine($"  add     -> field present: {table.FindField(fieldName) is not null}");
            table.DeleteField(fieldName);
            Console.WriteLine($"  delete  -> field present: {table.FindField(fieldName) is not null}");
        }
        catch (EsriGdbException e)
        {
            Console.WriteLine($"  schema change is not possible here: {e.Message}");
        }
    }

    // ---------------------------------------------------------------------
    // Domains + geodatabase management (read only here)
    // ---------------------------------------------------------------------
    Console.WriteLine();
    Console.WriteLine("Domains:");
    IReadOnlyList<string> domains = geodatabase.GetDomains();
    if (domains.Count == 0)
    {
        Console.WriteLine("  (none)");
    }

    foreach (string domain in domains.Take(3))
    {
        string definition = geodatabase.GetDomainDefinition(domain);
        Console.WriteLine($"  {domain}: {definition.Length} bytes of XML");
    }

    Console.WriteLine();
    Console.WriteLine("Create / compact / delete a geodatabase (in a temp folder):");
    RunManagementDemo();
}
catch (EsriGdbException e)
{
    Console.Error.WriteLine($"Error ({e.ErrorCode}): {e.Message}");
    Environment.ExitCode = 1;
}

// Creates a fresh geodatabase in the temp directory, adds a table to it,
// inserts a row, compacts and finally deletes it again.
static void RunManagementDemo()
{
    string path = Path.Combine(Path.GetTempPath(), "EsriGdbWrapperSample.gdb");

    try
    {
        if (Directory.Exists(path))
        {
            Geodatabase.Delete(path);
        }

        using (Geodatabase gdb = Geodatabase.Create(path))
        {
            Console.WriteLine($"  created     {path}");

            using Table table = gdb.CreateTable(TableDefinitionXml());
            Console.WriteLine($"  created     table '{table.Path}' with {table.Fields.Count} fields");

            table.AddField(
                "<Field><Name>NOTES</Name><Type>esriFieldTypeString</Type><IsNullable>true</IsNullable>" +
                "<Length>40</Length><Precision>0</Precision><Scale>0</Scale>" +
                "<AliasName>Notes</AliasName><ModelName>NOTES</ModelName></Field>");
            Console.WriteLine($"  add field   NOTES -> present: {table.FindField("NOTES") is not null}");

            int oid = table.InsertRow(new Dictionary<string, object?>
            {
                ["Label"] = "hello from EsriGdb",
                ["NOTES"] = null,                        // null -> SQL NULL
            });
            Console.WriteLine($"  insert      OBJECTID {oid}");

            using (QueryCursor cursor = table.Search(whereClause: $"OBJECTID = {oid}"))
            {
                Row? row = cursor.ReadNext();
                Console.WriteLine($"  read back   Label={row?.GetString("Label")}, NOTES is NULL: {row?.IsNull("NOTES")}");
            }

            table.DeleteField("NOTES");
            Console.WriteLine($"  drop field  NOTES -> present: {table.FindField("NOTES") is not null}");

            table.Close();
            gdb.Compact();
            Console.WriteLine("  compact     ok");
        }

        Geodatabase.Delete(path);
        Console.WriteLine($"  deleted     {path}");
    }
    catch (EsriGdbException e)
    {
        Console.WriteLine($"  management demo failed: {e.Message}");
        try
        {
            if (Directory.Exists(path))
            {
                Geodatabase.Delete(path);
            }
        }
        catch (EsriGdbException)
        {
            // Nothing else we can do here.
        }
    }
}

static string TableDefinitionXml() =>
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" +
    "<esri:DataElement xsi:type=\"esri:DETable\"" +
    " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"" +
    " xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"" +
    " xmlns:esri=\"http://www.esri.com/schemas/ArcGIS/10.1\">" +
    "<CatalogPath>\\DemoTable</CatalogPath>" +
    "<Name>DemoTable</Name>" +
    "<DatasetType>esriDTTable</DatasetType>" +
    "<HasOID>true</HasOID>" +
    "<OIDFieldName>OBJECTID</OIDFieldName>" +
    "<Fields xsi:type=\"esri:Fields\"><FieldArray xsi:type=\"esri:ArrayOfField\">" +
    "<Field xsi:type=\"esri:Field\">" +
    "<Name>OBJECTID</Name><Type>esriFieldTypeOID</Type><IsNullable>false</IsNullable>" +
    "<Length>4</Length><Precision>0</Precision><Scale>0</Scale>" +
    "<Required>true</Required><Editable>false</Editable>" +
    "<AliasName>OBJECTID</AliasName><ModelName>OBJECTID</ModelName>" +
    "</Field>" +
    "<Field xsi:type=\"esri:Field\">" +
    "<Name>Label</Name><Type>esriFieldTypeString</Type><IsNullable>true</IsNullable>" +
    "<Length>50</Length><Precision>0</Precision><Scale>0</Scale>" +
    "<AliasName>Label</AliasName><ModelName>Label</ModelName>" +
    "</Field>" +
    "</FieldArray></Fields>" +
    // <Indexes> has to be present and has to come before <CLSID>.
    "<Indexes xsi:type=\"esri:Indexes\">" +
    "<IndexArray xsi:type=\"esri:ArrayOfIndex\"/>" +
    "</Indexes>" +
    "<CLSID>{7A566981-C114-11D2-8A28-006097AFF44E}</CLSID>" +
    "</esri:DataElement>";

static string FormatValue(object? value) => value switch
{
    null => "NULL",
    byte[] bytes => $"<{bytes.Length} bytes>",
    double d => d.ToString("R", CultureInfo.InvariantCulture),
    float f => f.ToString("R", CultureInfo.InvariantCulture),
    DateTime dt => dt.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture),
    IFormattable formattable => formattable.ToString(null, CultureInfo.InvariantCulture),
    _ => value.ToString() ?? string.Empty,
};
