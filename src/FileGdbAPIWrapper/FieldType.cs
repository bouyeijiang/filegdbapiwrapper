namespace EsriGdb;

/// <summary>
/// Field types as defined by the Esri FileGDB API (FileGDBCore.h, FieldType).
/// </summary>
public enum FieldType
{
    SmallInteger = 0,
    Integer = 1,
    Single = 2,
    Double = 3,
    String = 4,
    Date = 5,
    OID = 6,
    Geometry = 7,
    Blob = 8,
    Raster = 9,
    GUID = 10,
    GlobalID = 11,
    XML = 12,
}
