namespace DarkVessel.Infrastructure;

public enum DesiredState
{
    Stopped,
    Running,
}

public sealed class CollectorState
{
    private readonly object _lock = new();

    public DesiredState Desired { get; private set; } = DesiredState.Stopped;
    public bool Connected { get; private set; }
    public long? CoverageId { get; private set; }
    public int Messages { get; private set; }
    public int RowsWritten { get; private set; }
    public DateTime? StartedAt { get; private set; }
    public DateTime? LastMessageAt { get; private set; }
    public string? LastError { get; private set; }

    public void RequestStart()
    {
        lock (_lock)
        {
            Desired = DesiredState.Running;
        }
    }

    public void RequestStop()
    {
        lock (_lock)
        {
            Desired = DesiredState.Stopped;
        }
    }

    public void OnSessionOpened(long coverageId)
    {
        lock (_lock)
        {
            CoverageId = coverageId;
            StartedAt = DateTime.UtcNow;
            Connected = true;
            Messages = 0;
            RowsWritten = 0;
            LastError = null;
        }
    }

    public void OnSessionClosed()
    {
        lock (_lock)
        {
            Connected = false;
            CoverageId = null;
        }
    }

    public void OnFlush(int messages, int rowsWrittenThisFlush, DateTime lastMessageAt)
    {
        lock (_lock)
        {
            Messages = messages;
            RowsWritten += rowsWrittenThisFlush;
            LastMessageAt = lastMessageAt;
        }
    }

    public void OnError(string message)
    {
        lock (_lock)
        {
            LastError = message;
            Connected = false;
        }
    }

    public StatusSnapshot Snapshot()
    {
        lock (_lock)
        {
            return new StatusSnapshot(Desired, Connected, CoverageId, Messages, RowsWritten, StartedAt, LastMessageAt, LastError);
        }
    }
}

public sealed record StatusSnapshot(
    DesiredState Desired,
    bool Connected,
    long? CoverageId,
    int Messages,
    int RowsWritten,
    DateTime? StartedAt,
    DateTime? LastMessageAt,
    string? LastError);
