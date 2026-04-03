import React, { useState } from 'react';

// DDNS Provider configuration wizard
// Used when adding a BYOR relay with optional dynamic DNS

export function DDNSWizard({ onComplete, onCancel }) {
    const [provider, setProvider] = useState(null);
    const [testing, setTesting] = useState(false);
    const [testResult, setTestResult] = useState(null);

    if (!provider) {
        return (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                <div style={{ color: 'var(--text)', fontWeight: 600, marginBottom: 4 }}>
                    DDNS Provider (Optional)
                </div>
                <div style={{ color: 'var(--muted)', fontSize: '0.85em', marginBottom: 8 }}>
                    Automatically update DNS when your IP changes.
                </div>
                <ProviderCard
                    name="Cloudflare"
                    label="Own domain — most reliable"
                    recommended
                    onClick={() => setProvider('cloudflare')}
                />
                <ProviderCard
                    name="deSEC"
                    label="Free — recommended"
                    onClick={() => setProvider('desec')}
                />
                <ProviderCard
                    name="DuckDNS"
                    label="Free — community/experimental"
                    experimental
                    onClick={() => setProvider('duckdns')}
                />
                <ProviderCard
                    name="Manual"
                    label="Enter hostname directly"
                    onClick={() => setProvider('manual')}
                />
                <button
                    onClick={onCancel}
                    style={{
                        background: 'transparent',
                        border: '1px solid var(--line)',
                        color: 'var(--muted)',
                        padding: '6px 12px',
                        borderRadius: 4,
                        cursor: 'pointer',
                        marginTop: 4,
                        fontSize: '0.85em',
                    }}
                >
                    Skip — no DDNS
                </button>
            </div>
        );
    }

    const ProviderForm = PROVIDER_FORMS[provider];
    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                <button
                    onClick={() => { setProvider(null); setTestResult(null); }}
                    style={{
                        background: 'transparent',
                        border: 'none',
                        color: 'var(--accent)',
                        cursor: 'pointer',
                        padding: 0,
                        fontSize: '0.85em',
                    }}
                >
                    ← Back
                </button>
                <span style={{ color: 'var(--text)', fontWeight: 600 }}>
                    {provider.charAt(0).toUpperCase() + provider.slice(1)} Setup
                </span>
            </div>
            <ProviderForm
                onComplete={(config) => onComplete({ provider, ...config })}
                testing={testing}
                testResult={testResult}
            />
        </div>
    );
}

function ProviderCard({ name, label, recommended, experimental, onClick }) {
    return (
        <button
            onClick={onClick}
            style={{
                display: 'flex',
                justifyContent: 'space-between',
                alignItems: 'center',
                background: 'var(--bg-elev)',
                border: '1px solid var(--line)',
                borderRadius: 6,
                padding: '10px 14px',
                cursor: 'pointer',
                textAlign: 'left',
                width: '100%',
            }}
        >
            <div>
                <div style={{ color: 'var(--text)', fontWeight: 500 }}>{name}</div>
                <div style={{ color: 'var(--muted)', fontSize: '0.8em' }}>{label}</div>
            </div>
            {recommended && (
                <span style={{
                    background: 'var(--accent)',
                    color: 'var(--bg)',
                    fontSize: '0.7em',
                    padding: '2px 6px',
                    borderRadius: 3,
                    fontWeight: 600,
                }}>
                    RECOMMENDED
                </span>
            )}
            {experimental && (
                <span style={{
                    background: 'var(--warn)',
                    color: 'var(--bg)',
                    fontSize: '0.7em',
                    padding: '2px 6px',
                    borderRadius: 3,
                    fontWeight: 600,
                }}>
                    EXPERIMENTAL
                </span>
            )}
        </button>
    );
}

function FieldInput({ label, value, onChange, type = 'text', placeholder, suffix }) {
    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
            <label style={{ color: 'var(--muted)', fontSize: '0.8em' }}>{label}</label>
            <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
                <input
                    type={type}
                    value={value}
                    onChange={(e) => onChange(e.target.value)}
                    placeholder={placeholder}
                    style={{
                        flex: 1,
                        background: 'var(--bg)',
                        border: '1px solid var(--line)',
                        borderRadius: 4,
                        padding: '6px 10px',
                        color: 'var(--text)',
                        fontSize: '0.85em',
                    }}
                />
                {suffix && <span style={{ color: 'var(--muted)', fontSize: '0.85em' }}>{suffix}</span>}
            </div>
        </div>
    );
}

function SaveButton({ onClick, disabled, label = 'Save' }) {
    return (
        <button
            onClick={onClick}
            disabled={disabled}
            style={{
                background: disabled ? 'var(--line)' : 'var(--accent)',
                color: disabled ? 'var(--muted)' : 'var(--bg)',
                border: 'none',
                borderRadius: 4,
                padding: '8px 16px',
                cursor: disabled ? 'not-allowed' : 'pointer',
                fontWeight: 600,
                fontSize: '0.85em',
                marginTop: 4,
            }}
        >
            {label}
        </button>
    );
}

function CloudflareForm({ onComplete }) {
    const [apiToken, setApiToken] = useState('');
    const [zoneId, setZoneId] = useState('');
    const [subdomain, setSubdomain] = useState('');
    const [domain, setDomain] = useState('');

    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
            <div style={{ color: 'var(--muted)', fontSize: '0.8em' }}>
                Requires a Cloudflare account and domain (~$10/yr).
            </div>
            <FieldInput label="API Token" value={apiToken} onChange={setApiToken} type="password" placeholder="cf_..." />
            <FieldInput label="Zone ID" value={zoneId} onChange={setZoneId} placeholder="32-character hex" />
            <FieldInput label="Domain" value={domain} onChange={setDomain} placeholder="example.com" />
            <FieldInput label="Subdomain" value={subdomain} onChange={setSubdomain} placeholder="relay" suffix={'.' + (domain || 'example.com')} />
            <SaveButton
                onClick={() => onComplete({ apiToken, zoneId, domain, subdomain, hostname: subdomain + '.' + domain })}
                disabled={!apiToken || !zoneId || !domain || !subdomain}
            />
        </div>
    );
}

function DeSECForm({ onComplete }) {
    const [token, setToken] = useState('');
    const [domain, setDomain] = useState('');
    const [subdomain, setSubdomain] = useState('');

    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
            <div style={{ color: 'var(--muted)', fontSize: '0.8em' }}>
                Free DNS from deSEC.io (EU nonprofit). Create an account at desec.io first.
            </div>
            <FieldInput label="API Token" value={token} onChange={setToken} type="password" />
            <FieldInput label="Domain" value={domain} onChange={setDomain} placeholder="mydomain.dedyn.io" />
            <FieldInput label="Subdomain (optional)" value={subdomain} onChange={setSubdomain} placeholder="relay" suffix={'.' + (domain || 'mydomain.dedyn.io')} />
            <SaveButton
                onClick={() => onComplete({
                    token, domain, subdomain,
                    hostname: subdomain ? subdomain + '.' + domain : domain,
                })}
                disabled={!token || !domain}
            />
        </div>
    );
}

function DuckDNSForm({ onComplete }) {
    const [token, setToken] = useState('');
    const [subdomain, setSubdomain] = useState('');

    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
            <div style={{
                background: 'rgba(255, 204, 102, 0.1)',
                border: '1px solid var(--warn)',
                borderRadius: 4,
                padding: 8,
                color: 'var(--warn)',
                fontSize: '0.8em',
            }}>
                ⚠ DuckDNS has had reliability issues (multi-day outages).
                Consider Cloudflare or deSEC for production use.
            </div>
            <FieldInput label="DuckDNS Token" value={token} onChange={setToken} type="password" />
            <FieldInput label="Subdomain" value={subdomain} onChange={setSubdomain} suffix=".duckdns.org" />
            <SaveButton
                onClick={() => onComplete({ token, subdomain, hostname: subdomain + '.duckdns.org' })}
                disabled={!token || !subdomain}
            />
        </div>
    );
}

function ManualForm({ onComplete }) {
    const [hostname, setHostname] = useState('');

    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
            <div style={{ color: 'var(--muted)', fontSize: '0.8em' }}>
                Enter your relay hostname directly. You manage DNS updates yourself.
            </div>
            <FieldInput label="Hostname" value={hostname} onChange={setHostname} placeholder="relay.example.com" />
            <SaveButton
                onClick={() => onComplete({ hostname })}
                disabled={!hostname}
            />
        </div>
    );
}

const PROVIDER_FORMS = {
    cloudflare: CloudflareForm,
    desec: DeSECForm,
    duckdns: DuckDNSForm,
    manual: ManualForm,
};
