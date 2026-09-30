using System.Collections;

namespace EsriGdb;

/// <summary>
/// The complete result of a SQL statement: column metadata plus all rows.
/// Statements that produce no result set (INSERT/UPDATE/DELETE/DDL) yield an
/// empty result.
/// </summary>
public sealed class QueryResult : IReadOnlyList<Row>
{
    private readonly Row[] _rows;

    internal QueryResult(Field[] fields, Row[] rows)
    {
        Fields = fields;
        _rows = rows;
    }

    /// <summary>Column metadata in result order.</summary>
    public IReadOnlyList<Field> Fields { get; }

    /// <summary>Number of returned rows.</summary>
    public int Count => _rows.Length;

    /// <summary>Zero based row accessor.</summary>
    public Row this[int index] => _rows[index];

    /// <summary>All rows as dictionaries keyed by column name.</summary>
    public IEnumerable<Dictionary<string, object?>> AsDictionaries()
        => _rows.Select(row => row.ToDictionary());

    public IEnumerator<Row> GetEnumerator() => ((IEnumerable<Row>)_rows).GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
}
