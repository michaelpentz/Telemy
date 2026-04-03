import React, { useState, useEffect, useCallback } from 'react';
import { DDNSWizard } from './ddns-wizard.jsx';

// Main relay add flow — shown when user clicks "+ Add Relay"
export function AddRelayFlow({ onAction, onCancel, onComplete }) {
    const [step, setStep] = useState('select_mode');
    const [mode, setMode] = useState(null);
    const [diagnosticState, setDiagnosticState] = useState(null);

    if (step === 'select_mode') {
        return (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 10, padding: 12 }}>
                <div style={{ color: 'var(--text)', fontWeight: 600, fontSize: '1em' }}>
                    How do you want to relay?
                </div>

                <RelayModeCard
                    title="Self-Hosted"
                    badge="Free"
                    badgeColor="var(--good)"
                    description="Stream through your home PC"
                    details={[
                        { icon: '✓', text: 'No monthly cost', color: 'var(--good)' },
                        { icon: '✓', text: 'Your infrastructure', color: 'var(--good)' },
                        { icon: '⚠', text: 'Requires port forwarding', color: 'var(--warn)' },
                        { icon: '⚠', text: 'Home PC must be on', color: 'var(--warn)' },
                    ]}
                    onClick={() => { setMode('self_hosted'); setStep('diagnostics'); }}
                />

                <RelayModeCard
                    title="Managed Cloud"
                    badge="$9.99/mo"
                    badgeColor="var(--accent)"
                    description="Stream through Telemy's servers"
                    details={[
                        { icon: '✓', text: 'Works anywhere, nothing to configure', color: 'var(--good)' },
                        { icon: '✓', text: 'Auto-reconnect and BRB fallback', color: 'var(--good)' },
                        { icon: '✓', text: 'Multi-encoder support', color: 'var(--good)' },
                    ]}
                    onClick={() => { setMode('managed'); setStep('configure_managed'); }}
                />

                <RelayModeCard
                    title="Bring Your Own Relay"
                    badge="Advanced"
                    badgeColor="var(--muted)"
                    description="Point at your own SRT/SRTLA server"
                    details={[
                        { icon: '→', text: 'For users with existing relay infrastructure', color: 'var(--muted)' },
                    ]}
                    onClick={() => { setMode('byor'); setStep('configure_byor'); }}
                />

                <button onClick={onCancel} style={{
                    background: 'transparent', border: '1px solid var(--line)',
                    color: 'var(--muted)', padding: '6px 12px', borderRadius: 4,
                    cursor: 'pointer', fontSize: '0.85em', marginTop: 4,
                }}>Cancel</button>
            </div>
        );
    }

    if (step === 'diagnostics') {
        return <SelfHostedDiagnostics
            onAction={onAction}
            onComplete={(result) => { setDiagnosticState(result); setStep('diagnostic_result'); }}
            onCancel={() => setStep('select_mode')}
        />;
    }

    if (step === 'diagnostic_result' && diagnosticState) {
        return <DiagnosticResult
            result={diagnosticState}
            onSwitchToManaged={() => { setMode('managed'); setStep('configure_managed'); }}
            onDone={onComplete}
            onRetry={() => setStep('diagnostics')}
        />;
    }

    if (step === 'configure_managed') {
        // For managed mode — just needs region selection, handled by existing relay_start action
        return (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 10, padding: 12 }}>
                <div style={{ color: 'var(--text)', fontWeight: 600 }}>Managed Cloud Relay</div>
                <div style={{ color: 'var(--muted)', fontSize: '0.85em' }}>
                    Click Start to provision a managed relay. Region will be auto-selected.
                </div>
                <div style={{ display: 'flex', gap: 8 }}>
                    <button onClick={() => {
                        onAction({ type: 'relay_start', requestId: 'add_managed_' + Date.now() });
                        onComplete && onComplete();
                    }} style={{
                        background: 'var(--accent)', color: 'var(--bg)', border: 'none',
                        borderRadius: 4, padding: '8px 16px', cursor: 'pointer', fontWeight: 600,
                    }}>Start Relay</button>
                    <button onClick={() => setStep('select_mode')} style={{
                        background: 'transparent', border: '1px solid var(--line)',
                        color: 'var(--muted)', padding: '8px 16px', borderRadius: 4, cursor: 'pointer',
                    }}>Back</button>
                </div>
            </div>
        );
    }

    if (step === 'configure_byor') {
        return <BYORSetup onAction={onAction} onCancel={() => setStep('select_mode')} onComplete={onComplete} />;
    }

    return null;
}

function RelayModeCard({ title, badge, badgeColor, description, details, onClick }) {
    return (
        <button onClick={onClick} style={{
            display: 'flex', flexDirection: 'column', gap: 6,
            background: 'var(--bg-elev)', border: '1px solid var(--line)',
            borderRadius: 8, padding: '12px 14px', cursor: 'pointer',
            textAlign: 'left', width: '100%', transition: 'border-color 0.15s',
        }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
                <span style={{ color: 'var(--text)', fontWeight: 600 }}>{title}</span>
                <span style={{
                    background: badgeColor, color: 'var(--bg)',
                    fontSize: '0.7em', padding: '2px 8px', borderRadius: 3, fontWeight: 700,
                }}>{badge}</span>
            </div>
            <div style={{ color: 'var(--muted)', fontSize: '0.85em' }}>{description}</div>
            {details && details.length > 0 && (
                <div style={{ display: 'flex', flexDirection: 'column', gap: 2, marginTop: 2 }}>
                    {details.map((d, i) => (
                        <div key={i} style={{ color: d.color, fontSize: '0.8em', display: 'flex', gap: 6 }}>
                            <span>{d.icon}</span>
                            <span>{d.text}</span>
                        </div>
                    ))}
                </div>
            )}
        </button>
    );
}

// Self-hosted diagnostic steps
export function SelfHostedDiagnostics({ onAction, onComplete, onCancel }) {
    const [steps, setSteps] = useState([
        { id: 'srtla', label: 'Starting relay listener', status: 'pending', detail: '' },
        { id: 'upnp', label: 'Opening port on router (UPnP)', status: 'pending', detail: '' },
        { id: 'probe', label: 'Checking reachability from internet', status: 'pending', detail: '' },
        { id: 'bandwidth', label: 'Checking bandwidth', status: 'pending', detail: '' },
    ]);
    const [started, setStarted] = useState(false);

    const updateStep = useCallback((id, updates) => {
        setSteps(prev => prev.map(s => s.id === id ? { ...s, ...updates } : s));
    }, []);

    useEffect(() => {
        if (started) return;
        setStarted(true);

        // Send self_hosted_start action — the C++ side generates the connection_id,
        // runs the diagnostic pipeline, and emits progress updates as dock action results
        onAction({
            type: 'self_hosted_start',
            requestId: 'diag_' + Date.now(),
        });

        // Simulate step progress for now — in production, these come from dock action results
        const runSteps = async () => {
            updateStep('srtla', { status: 'running' });
            await delay(1500);
            updateStep('srtla', { status: 'pass', detail: 'Port 5000' });

            updateStep('upnp', { status: 'running' });
            await delay(2000);
            updateStep('upnp', { status: 'pass', detail: 'Port mapped via UPnP' });

            updateStep('probe', { status: 'running' });
            await delay(2500);
            updateStep('probe', { status: 'pass', detail: 'Reachable' });

            updateStep('bandwidth', { status: 'running' });
            await delay(2000);
            updateStep('bandwidth', { status: 'pass', detail: 'Headroom: 26 Mbps' });

            onComplete({
                success: true,
                fqdn: 'abc123.relay.telemyapp.com',
                port: 5000,
            });
        };
        runSteps();
    }, [started, onAction, onComplete, updateStep]);

    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 10, padding: 12 }}>
            <div style={{ color: 'var(--text)', fontWeight: 600 }}>Setting up Self-Hosted Relay</div>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
                {steps.map(step => (
                    <DiagnosticStep key={step.id} {...step} />
                ))}
            </div>
            <button onClick={onCancel} style={{
                background: 'transparent', border: '1px solid var(--line)',
                color: 'var(--muted)', padding: '6px 12px', borderRadius: 4,
                cursor: 'pointer', fontSize: '0.85em', marginTop: 4, alignSelf: 'flex-start',
            }}>Cancel</button>
        </div>
    );
}

function DiagnosticStep({ label, status, detail }) {
    const icons = { pending: '○', running: '◌', pass: '✓', fail: '✗' };
    const colors = {
        pending: 'var(--muted)',
        running: 'var(--accent)',
        pass: 'var(--good)',
        fail: 'var(--danger)',
    };

    return (
        <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
            <span style={{
                color: colors[status],
                fontWeight: status === 'running' ? 700 : 400,
                width: 14, textAlign: 'center',
            }}>{icons[status]}</span>
            <span style={{ color: status === 'pending' ? 'var(--muted)' : 'var(--text)', fontSize: '0.9em' }}>
                {label}
            </span>
            {detail && (
                <span style={{ color: 'var(--muted)', fontSize: '0.8em', marginLeft: 'auto' }}>{detail}</span>
            )}
        </div>
    );
}

function DiagnosticResult({ result, onSwitchToManaged, onDone, onRetry }) {
    if (result.success) {
        return (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 10, padding: 12 }}>
                <div style={{ color: 'var(--good)', fontWeight: 600, fontSize: '1em' }}>
                    ✓ Your self-hosted relay is ready!
                </div>
                <div style={{
                    background: 'var(--bg-elev)', border: '1px solid var(--line)',
                    borderRadius: 6, padding: 12, fontFamily: 'monospace', fontSize: '0.9em',
                }}>
                    <div style={{ color: 'var(--text)' }}>{result.fqdn}:{result.port}</div>
                </div>
                <div style={{ color: 'var(--muted)', fontSize: '0.85em' }}>
                    Point your IRL encoder (IRL Pro, Belabox, etc.) at this address.
                </div>
                <button onClick={onDone} style={{
                    background: 'var(--accent)', color: 'var(--bg)', border: 'none',
                    borderRadius: 4, padding: '8px 16px', cursor: 'pointer', fontWeight: 600,
                }}>Done</button>
            </div>
        );
    }

    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 10, padding: 12 }}>
            <div style={{ color: 'var(--danger)', fontWeight: 600 }}>
                ✗ Could not set up self-hosted relay
            </div>
            <div style={{ color: 'var(--muted)', fontSize: '0.85em' }}>
                {result.error || 'Your ISP may block inbound connections.'}
            </div>
            <div style={{ display: 'flex', gap: 8 }}>
                <button onClick={onRetry} style={{
                    background: 'transparent', border: '1px solid var(--line)',
                    color: 'var(--text)', padding: '8px 16px', borderRadius: 4, cursor: 'pointer',
                }}>Try Again</button>
                <button onClick={onSwitchToManaged} style={{
                    background: 'var(--accent)', color: 'var(--bg)', border: 'none',
                    borderRadius: 4, padding: '8px 16px', cursor: 'pointer', fontWeight: 600,
                }}>Switch to Managed Cloud</button>
            </div>
        </div>
    );
}

function BYORSetup({ onAction, onCancel, onComplete }) {
    const [host, setHost] = useState('');
    const [port, setPort] = useState('5000');
    const [streamId, setStreamId] = useState('');
    const [showDDNS, setShowDDNS] = useState(false);

    if (showDDNS) {
        return <DDNSWizard
            onComplete={(ddnsConfig) => {
                // Save DDNS config alongside BYOR connection
                setShowDDNS(false);
            }}
            onCancel={() => setShowDDNS(false)}
        />;
    }

    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 10, padding: 12 }}>
            <div style={{ color: 'var(--text)', fontWeight: 600 }}>Bring Your Own Relay</div>
            <FieldInput label="Relay Host" value={host} onChange={setHost} placeholder="relay.example.com" />
            <FieldInput label="Port" value={port} onChange={setPort} placeholder="5000" />
            <FieldInput label="Stream ID (optional)" value={streamId} onChange={setStreamId} />
            <button onClick={() => setShowDDNS(true)} style={{
                background: 'transparent', border: '1px solid var(--line)',
                color: 'var(--accent)', padding: '6px 12px', borderRadius: 4,
                cursor: 'pointer', fontSize: '0.85em', alignSelf: 'flex-start',
            }}>Set up DDNS (optional)</button>
            <div style={{ display: 'flex', gap: 8, marginTop: 4 }}>
                <button onClick={() => {
                    onAction({
                        type: 'relay_connect_direct',
                        requestId: 'byor_' + Date.now(),
                        relay_host: host,
                        relay_port: parseInt(port) || 5000,
                        stream_id: streamId,
                    });
                    onComplete && onComplete();
                }} disabled={!host} style={{
                    background: !host ? 'var(--line)' : 'var(--accent)',
                    color: !host ? 'var(--muted)' : 'var(--bg)',
                    border: 'none', borderRadius: 4, padding: '8px 16px',
                    cursor: !host ? 'not-allowed' : 'pointer', fontWeight: 600,
                }}>Connect</button>
                <button onClick={onCancel} style={{
                    background: 'transparent', border: '1px solid var(--line)',
                    color: 'var(--muted)', padding: '8px 16px', borderRadius: 4, cursor: 'pointer',
                }}>Back</button>
            </div>
        </div>
    );
}

function FieldInput({ label, value, onChange, placeholder, type = 'text' }) {
    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
            <label style={{ color: 'var(--muted)', fontSize: '0.8em' }}>{label}</label>
            <input type={type} value={value} onChange={e => onChange(e.target.value)}
                placeholder={placeholder} style={{
                    background: 'var(--bg)', border: '1px solid var(--line)',
                    borderRadius: 4, padding: '6px 10px', color: 'var(--text)', fontSize: '0.85em',
                }} />
        </div>
    );
}

function delay(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}
