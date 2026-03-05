import { createRoot } from "react-dom/client";
import AegisDock from "./aegis-dock.jsx";

try {
  const root = document.getElementById("root");
  if (!root) {
    document.body.innerHTML = '<pre style="color:red;padding:10px">ERROR: #root element not found</pre>';
  } else {
    createRoot(root).render(<AegisDock />);
  }
} catch (err) {
  document.body.innerHTML = '<pre style="color:red;padding:10px;white-space:pre-wrap">MOUNT ERROR: ' +
    String(err && err.stack || err) + '</pre>';
}
