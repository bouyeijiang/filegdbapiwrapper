using System.Globalization;

namespace EsriGdb;

/// <summary>
/// Converts the UTF-8 text produced by the native bridge into CLR values based
/// on the FileGDB field type. When a value cannot be parsed it is returned as
/// the original string so that no information is lost.
/// </summary>
internal static class ValueConverter
{
    internal static object? Parse(FieldType fieldType, string? utf8Value)
    {
        if (utf8Value is null)
        {
            return null; // SQL NULL
        }

        try
        {
            return fieldType switch
            {
                FieldType.SmallInteger => short.Parse(utf8Value, NumberStyles.Integer, CultureInfo.InvariantCulture),
                FieldType.Integer => int.Parse(utf8Value, NumberStyles.Integer, CultureInfo.InvariantCulture),
                FieldType.OID => int.Parse(utf8Value, NumberStyles.Integer, CultureInfo.InvariantCulture),
                FieldType.Single => float.Parse(utf8Value, NumberStyles.Float, CultureInfo.InvariantCulture),
                FieldType.Double => double.Parse(utf8Value, NumberStyles.Float, CultureInfo.InvariantCulture),
                FieldType.Date => DateTime.ParseExact(
                    utf8Value, "yyyy-MM-dd'T'HH:mm:ss", CultureInfo.InvariantCulture,
                    DateTimeStyles.None),
                FieldType.GUID => Guid.Parse(utf8Value),
                FieldType.GlobalID => Guid.Parse(utf8Value),
                FieldType.Blob => System.Convert.FromBase64String(utf8Value),
                FieldType.Geometry => System.Convert.FromBase64String(utf8Value),
                _ => utf8Value, // String, XML, Raster and anything unknown
            };
        }
        catch (Exception e) when (e is FormatException or OverflowException or ArgumentException)
        {
            return utf8Value;
        }
    }

    /// <summary>
    /// Formats a CLR value as the UTF-8 text the native bridge expects for a
    /// field. Returns <c>null</c> for SQL NULL. The native layer parses the
    /// text according to the field type, so most values pass through unchanged.
    /// </summary>
    /// <exception cref="ArgumentException">The value cannot be represented as text.</exception>
    internal static string? Format(object? value)
    {
        switch (value)
        {
            case null:
                return null;

            case string text:
                return text;

            case byte[] bytes:
                return Convert.ToBase64String(bytes);   // Blob / ShapeBuffer

            case DateTime date:
                return date.ToString("yyyy-MM-dd'T'HH:mm:ss", CultureInfo.InvariantCulture);

            case DateTimeOffset offset:
                return offset.DateTime.ToString("yyyy-MM-dd'T'HH:mm:ss", CultureInfo.InvariantCulture);

            case Guid guid:
                return guid.ToString("D");

            case bool flag:
                return flag ? "1" : "0";

            case char c:
                return c.ToString();

            case IFormattable formattable:
                return formattable.ToString(null, CultureInfo.InvariantCulture);

            default:
                return value.ToString();
        }
    }
}
