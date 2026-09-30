using System.Collections;
using System.Globalization;

namespace EsriGdb;

/// <summary>
/// A single result row. Values are converted according to the column's
/// <see cref="EsriGdb.FieldType"/>; SQL NULL is represented by <c>null</c>.
/// </summary>
public sealed class Row : IReadOnlyList<object?>
{
    private readonly Field[] _fields;
    private readonly object?[] _values;
    private readonly Dictionary<string, int> _nameToIndex;

    internal Row(Field[] fields, object?[] values, Dictionary<string, int> nameToIndex)
    {
        _fields = fields;
        _values = values;
        _nameToIndex = nameToIndex;
    }

    /// <summary>Column metadata in result order.</summary>
    public IReadOnlyList<Field> Fields => _fields;

    /// <summary>Number of cells in the row.</summary>
    public int Count => _values.Length;

    /// <summary>Zero based cell accessor.</summary>
    public object? this[int index] => _values[index];

    /// <summary>Cell accessor by column name (case sensitive).</summary>
    public object? this[string name] => _values[IndexOf(name)];

    /// <summary>Returns the cell converted to <typeparamref name="T"/>, or <c>default</c>.</summary>
    public T? GetValue<T>(int index)
    {
        object? value = this[index];
        if (value is null)
        {
            return default;
        }

        if (value is T typed)
        {
            return typed;
        }

        // Tolerate conversions such as string -> DateTime / number.
        return (T?)Convert.ChangeType(value, typeof(T), CultureInfo.InvariantCulture);
    }

    /// <summary>Returns the cell converted to <typeparamref name="T"/>, or <c>default</c>.</summary>
    public T? GetValue<T>(string name) => GetValue<T>(IndexOf(name));

    public string? GetString(string name) => GetValue<string>(name);
    public int? GetInt32(string name) => GetValue<int>(name);
    public short? GetInt16(string name) => GetValue<short>(name);
    public float? GetSingle(string name) => GetValue<float>(name);
    public double? GetDouble(string name) => GetValue<double>(name);
    public DateTime? GetDateTime(string name) => GetValue<DateTime>(name);
    public Guid? GetGuid(string name) => GetValue<Guid>(name);
    public byte[]? GetBytes(string name) => GetValue<byte[]>(name);

    /// <summary>True when the cell holds SQL NULL.</summary>
    public bool IsNull(int index) => this[index] is null;

    /// <summary>True when the cell holds SQL NULL.</summary>
    public bool IsNull(string name) => this[IndexOf(name)] is null;

    /// <summary>Copies the row into a dictionary keyed by column name.</summary>
    public Dictionary<string, object?> ToDictionary()
    {
        var result = new Dictionary<string, object?>(_fields.Length, StringComparer.Ordinal);
        for (int i = 0; i < _fields.Length; i++)
        {
            result[_fields[i].Name] = _values[i];
        }

        return result;
    }

    private int IndexOf(string name)
    {
        if (_nameToIndex.TryGetValue(name, out int index))
        {
            return index;
        }

        throw new ArgumentException(
            $"Column '{name}' was not returned by the query. Known columns: {string.Join(", ", _nameToIndex.Keys)}.",
            nameof(name));
    }

    public IEnumerator<object?> GetEnumerator() => ((IEnumerable<object?>)_values).GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
}
