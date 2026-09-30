namespace EsriGdb;

/// <summary>
/// Thrown when a FileGDB operation fails. <see cref="ErrorCode"/> holds the raw
/// fgdbError value returned by the native Esri FileGDB API (or -1/-2 for bridge
/// level argument/internal errors).
/// </summary>
public sealed class EsriGdbException : Exception
{
    public EsriGdbException(string message, int errorCode)
        : base(message)
    {
        ErrorCode = errorCode;
    }

    public EsriGdbException(string message, int errorCode, Exception innerException)
        : base(message, innerException)
    {
        ErrorCode = errorCode;
    }

    /// <summary>Raw error code reported by the native layer.</summary>
    public int ErrorCode { get; }
}
