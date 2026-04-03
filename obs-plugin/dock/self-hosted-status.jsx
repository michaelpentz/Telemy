import React, { useState } from 'react';

// Row component for a self-hosted relay in the connection list
export function SelfHostedRelayRow({ relay, onAction }) {
    const [expanded, setExpanded] = useState(false);

    const statusColor = relay.status === 'live'
        ? 'var(--good)'
        : relay.status === 'ready'
            ? 'var(--good)'
            : relay.status === 'error'
                ? 'var(--danger)'
                : 'var(--warn)';

    return (
        <div style={{
            background: 'var(--bg-elev)',
            border: '1px solid var(--line)',
            borderRadius: 6,
            overflow: 'hidden',
            marginBottom: 4,
        }}>
            {/* Collapsed header */}
            <button
                onClick={() => setExpanded(!expanded)}
                style={{
                    display: 'flex',
                    alignItems: 'center',
                    gap: 8,
                    width: '100%',
                    padding: '8px 12px',
                    background: 'transparent',
                    border: 'none',
                    cursor: 'pointer',
                    textAlign: 'left',
                }}
            >
                <span style={{
                    width: 8, height: 8, borderRadius: '50%',
                    background: statusColor, flexShrink: 0,
                }} />
                <span style={{ color: 'var(--text)', fontWeight: 500, flex: 1, fontSize: '0.9em' }}>
                    {relay.label || relay.id}
                </span>
                <span style={{
                    color: 'var(--muted)', fontSize: '0.75em',
                    background: 'rgba(111, 240, 168, 0.1)',
                    border: '1px solid rgba(111, 240, 168, 0.3)',
                    padding: '1px 6px', borderRadius: 3,
                }}>Self-Hosted</span>
                {relay.inbound_kbps > 0 && (
                    <span style={{ color: 'var(--accent)', fontSize: '0.85em', fontFamily: 'monospace' }}>
                        {(relay.inbound_kbps / 1000).toFixed(1)}M
                    </span>
                )}
                <span style={{
                    color: 'var(--muted)', fontSize: '0.7em',
                    transform: expanded ? 'rotate(180deg)' : 'rotate(0deg)',
                    transition: 'transform 0.15s',
                }}>▼</span>
            </button>

            {/* Expanded detail */}
            {expanded && (
                <div style={{
                    padding: '0 12px 10px 12px',
                    display: 'flex', flexDirection: 'column', gap: 6,
                    borderTop: '1px solid var(--line)',
                    paddingTop: 8,
                }}>
                    {/* Relay address */}
                    <DetailRow label="Address">
                        <span style={{ fontFamily: 'monospace', color: 'var(--text)', fontSize: '0.85em' }}>
                            {relay.fqdn}:{relay.port}
                        </span>
                        <CopyButton text={relay.fqdn + ':' + relay.port} />
                    </DetailRow>

                    {/* Connectivity */}
                    <DetailRow label="Status">
                        <span style={{
                            color: relay.connectivity === 'reachable' ? 'var(--good)'
                                : relay.connectivity === 'unreachable' ? 'var(--danger)'
                                : 'var(--warn)',
                            fontSize: '0.85em',
                        }}>
                            {relay.connectivity === 'reachable' ? '✓ Reachable'
                                : relay.connectivity === 'unreachable' ? '✗ Unreachable'
                                : '? Unknown'}
                        </span>
                    </DetailRow>

                    {/* UPnP */}
                    <DetailRow label="UPnP">
                        <span style={{
                            color: relay.upnp_active ? 'var(--good)' : 'var(--warn)',
                            fontSize: '0.85em',
                        }}>
                            {relay.upnp_active ? '✓ Port mapped' : '⚠ Manual port forward needed'}
                        </span>
                    </DetailRow>

                    {/* External IP */}
                    {relay.external_ip && (
                        <DetailRow label="External IP">
                            <span style={{ fontFamily: 'monospace', color: 'var(--muted)', fontSize: '0.85em' }}>
                                {relay.external_ip}
                            </span>
                        </DetailRow>
                    )}

                    {/* Per-link stats */}
                    {relay.links && relay.links.length > 0 && (
                        <div style={{ marginTop: 4 }}>
                            <div style={{ color: 'var(--muted)', fontSize: '0.75em', marginBottom: 4 }}>
                                Inbound Links ({relay.links.length})
                            </div>
                            {relay.links.map((link, i) => (
                                <div key={i} style={{
                                    display: 'flex', justifyContent: 'space-between',
                                    fontSize: '0.8em', padding: '2px 0',
                                }}>
                                    <span style={{ color: 'var(--muted)' }}>
                                        {link.asn_org || link.addr}
                                    </span>
                                    <span style={{ color: 'var(--text)', fontFamily: 'monospace' }}>
                                        {link.sharePct != null ? link.sharePct.toFixed(0) + '%' : ''}
                                    </span>
                                </div>
                            ))}
                        </div>
                    )}

                    {/* Bandwidth info */}
                    {relay.bandwidth_report && (
                        <BandwidthSummary report={relay.bandwidth_report} />
                    )}

                    {/* Actions */}
                    <div style={{ display: 'flex', gap: 8, marginTop: 6 }}>
                        <button onClick={() => {
                            onAction({
                                type: 'self_hosted_stop',
                                requestId: 'stop_' + relay.id + '_' + Date.now(),
                                connection_id: relay.id,
                            });
                        }} style={{
                            background: 'transparent',
                            border: '1px solid var(--danger)',
                            color: 'var(--danger)',
                            padding: '4px 12px',
                            borderRadius: 4,
                            cursor: 'pointer',
                            fontSize: '0.8em',
                        }}>Remove Relay</button>
                    </div>
                </div>
            )}
        </div>
    );
}

function BandwidthSummary({ report }) {
    if (!report) return null;

    const verdictColors = {
        plenty: 'var(--good)',
        ok: 'var(--good)',
        tight: 'var(--warn)',
        insufficient: 'var(--danger)',
        unknown: 'var(--muted)',
    };

    return (
        <div style={{
            background: 'var(--bg)',
            border: '1px solid var(--line)',
            borderRadius: 4,
            padding: 8,
            marginTop: 4,
        }}>
            <div style={{ color: 'var(--muted)', fontSize: '0.75em', marginBottom: 4 }}>Bandwidth</div>
            {report.download_mbps > 0 && (
                <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: '0.8em' }}>
                    <span style={{ color: 'var(--muted)' }}>Relay inbound (download):</span>
                    <span style={{ color: 'var(--good)' }}>{report.download_mbps} Mbps ✓</span>
                </div>
            )}
            <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: '0.8em' }}>
                <span style={{ color: 'var(--muted)' }}>Platform upload:</span>
                <span style={{ color: 'var(--text)' }}>{report.upload_mbps} Mbps</span>
            </div>
            <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: '0.8em' }}>
                <span style={{ color: 'var(--muted)' }}>OBS outputs:</span>
                <span style={{ color: 'var(--text)' }}>{(report.total_output_kbps / 1000).toFixed(1)} Mbps</span>
            </div>
            <div style={{
                display: 'flex', justifyContent: 'space-between', fontSize: '0.85em',
                marginTop: 4, paddingTop: 4, borderTop: '1px solid var(--line)',
            }}>
                <span style={{ color: verdictColors[report.verdict] }}>
                    Headroom: {report.headroom_mbps.toFixed(1)} Mbps
                </span>
                <span style={{ color: verdictColors[report.verdict] }}>
                    {report.verdict === 'plenty' || report.verdict === 'ok' ? '✓' : '⚠'}
                </span>
            </div>
        </div>
    );
}

function DetailRow({ label, children }) {
    return (
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
            <span style={{ color: 'var(--muted)', fontSize: '0.8em' }}>{label}</span>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                {children}
            </div>
        </div>
    );
}

function CopyButton({ text }) {
    const [copied, setCopied] = useState(false);

    return (
        <button onClick={() => {
            // Use CEF-safe clipboard method
            if (typeof cefCopyToClipboard === 'function') {
                cefCopyToClipboard(text);
            } else if (typeof window !== 'undefined' && window.telemyDockNative) {
                // Fallback via bridge
            }
            setCopied(true);
            setTimeout(() => setCopied(false), 1500);
        }} style={{
            background: 'transparent',
            border: '1px solid var(--line)',
            color: copied ? 'var(--good)' : 'var(--muted)',
            padding: '2px 6px',
            borderRadius: 3,
            cursor: 'pointer',
            fontSize: '0.75em',
        }}>
            {copied ? 'Copied' : 'Copy'}
        </button>
    );
}
