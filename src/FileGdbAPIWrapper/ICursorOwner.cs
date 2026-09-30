namespace EsriGdb;

/// <summary>
/// Something that owns query cursors: a <see cref="Geodatabase"/> (SQL cursors,
/// plus every cursor of its tables) or a <see cref="Table"/> (search cursors).
/// Closing the owner closes the cursors it still holds.
/// </summary>
internal interface ICursorOwner
{
    /// <summary>Lock protecting the cursor registry of this owner.</summary>
    object CursorSync { get; }

    /// <summary>Human readable owner kind, used in error messages.</summary>
    string OwnerKind { get; }

    /// <summary>Registers a cursor with this owner.</summary>
    void AddCursor(QueryCursor cursor);

    /// <summary>Removes a cursor (called when the cursor is disposed).</summary>
    void UnregisterCursor(QueryCursor cursor);

    /// <summary>Closes every cursor still registered with this owner.</summary>
    void CloseAllCursors();
}
