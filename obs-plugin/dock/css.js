// =============================================================================
// CSS
// =============================================================================

export function getDockCss(theme) {
  return `
  /* === OBS CEF host sizing — ensures height:100% propagates to the component === */
  html, body { height: 100%; margin: 0; padding: 0; overflow: hidden; }
  #root { height: 100%; }

  @keyframes pulse { 0%, 100% { transform: scale(1); opacity: 0.3; } 50% { transform: scale(2.2); opacity: 0; } }
  @keyframes slideIn { from { opacity: 0; transform: translateY(-4px); } to { opacity: 1; transform: translateY(0); } }
  @keyframes shimmer {
    0% { background-position: -200% 0; }
    100% { background-position: 200% 0; }
  }
  @keyframes railPulse {
    0%, 100% { opacity: 0.7; }
    50% { opacity: 1; }
  }
  @keyframes provisionPulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.5; }
  }
  @keyframes dotBlink {
    0%, 100% { opacity: 0.2; }
    50% { opacity: 1; }
  }
  .aegis-dock-scroll::-webkit-scrollbar { width: 3px; }
  .aegis-dock-scroll::-webkit-scrollbar-track { background: transparent; }
  .aegis-dock-scroll::-webkit-scrollbar-thumb { background: ${theme.scrollbar}; border-radius: 2px; }
  .aegis-dock-scroll::-webkit-scrollbar-thumb:hover { background: ${theme.border}; border-radius: 2px; }
`;
}
