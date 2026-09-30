using System.Globalization;

namespace EsriGdb;

/// <summary>
/// A rectangular spatial filter in the coordinate system of the feature class,
/// used by <see cref="Table.Search"/> to restrict a query to an area of interest.
/// </summary>
public readonly record struct Envelope(double XMin, double YMin, double XMax, double YMax)
{
    public override string ToString() =>
        string.Create(CultureInfo.InvariantCulture,
            $"[{XMin:F6}, {YMin:F6} .. {XMax:F6}, {YMax:F6}]");
}
