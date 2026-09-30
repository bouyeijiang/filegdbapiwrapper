namespace EsriGdb;

/// <summary>
/// Metadata of a single column: of a query result, of a cursor row, or of a
/// table field.
/// </summary>
public sealed class Field
{
    internal Field(int index, string name, FieldType type, int length = -1, bool isNullable = true)
    {
        Index = index;
        Name = name;
        Type = type;
        Length = length;
        IsNullable = isNullable;
    }

    /// <summary>Zero based column position.</summary>
    public int Index { get; }

    /// <summary>Column name, e.g. <c>CITY_NAME</c>.</summary>
    public string Name { get; }

    /// <summary>FileGDB field type.</summary>
    public FieldType Type { get; }

    /// <summary>Maximum length in characters (-1 when unknown, e.g. non textual fields).</summary>
    public int Length { get; }

    /// <summary>True when the field accepts NULL (true when unknown).</summary>
    public bool IsNullable { get; }

    public override string ToString() =>
        Length > 0 ? $"{Name} ({Type}, {Length})" : $"{Name} ({Type})";
}
