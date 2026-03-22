# Stream Slot System — Implementation Spec

**Date:** 2026-03-22
**Status:** Implemented (retroactive documentation)
**Commits:** `b706153`, `c70efe7`, `0e933aa`

---

## Overview

The Stream Slot system allows users to manage multiple concurrent relay connections through named, numbered slots. Each slot represents a logical managed relay connection that can be independently configured, started, and stopped. Slots are server-side constructs tied to the user's account — the dock UI presents them as a dropdown when adding or editing managed connections.

## Data Model

### `user_stream_slots` table

Stream slots are stored per-user in the control plane database:

| Column | Type | Description |
|--------|------|-------------|
| `user_id` | TEXT | FK to `users.id` |
| `slot_number` | INTEGER | Ordinal slot identifier (1-based) |
| `label` | TEXT | User-defined display name (e.g., "Main Camera") |
| `created_at` | TIMESTAMPTZ | Creation timestamp |

Primary key: `(user_id, slot_number)`

### Relationship to sessions

When a managed connection starts, the `connection_id` maps to a specific slot. The `sessions` table stores the `connection_id` which correlates to the slot on the client side. The `relay_assignments` table mirrors `connection_id` for pool server tracking.

## API Endpoints

### List Stream Slots

```
GET /api/v1/stream-slots
Authorization: Bearer {jwt}

Response 200:
{
  "slots": [
    { "slot_number": 1, "label": "Main Camera" },
    { "slot_number": 2, "label": "Backpack Cam" }
  ]
}
```

### Rename Stream Slot

```
PUT /api/v1/stream-slots/{slot_number}
Authorization: Bearer {jwt}

Request:
{ "label": "New Label" }

Response 200:
{ "slot_number": 1, "label": "New Label" }
```

### Relay Start (existing, slot-aware)

```
POST /api/v1/relay/start
Authorization: Bearer {jwt}

Request:
{
  "region_preference": "us-east",
  "connection_id": "uuid-v4",
  "stream_slot": 1
}
```

The `stream_slot` number is passed alongside `connection_id` to associate the session with a specific slot.

## Dock UI Integration

- **Add Connection Form**: Managed connections show a "Relay Slot" dropdown populated from the `/stream-slots` endpoint. Users select which slot to use for the connection.
- **Connection Expanded Detail**: Displays the slot label and number. An inline rename editor sends `rename_stream_slot` bridge action.
- **Edit Form**: Slot can be switched when editing an existing managed connection.
- **Bridge Actions**:
  - `rename_stream_slot` — updates label via PUT endpoint
  - `connection_add` with `stream_slot` field — associates new connection to slot

## Relationship to Connections

Stream slots are the server-side complement to the client-side multi-connection model:

- **BYOR connections**: Do not use stream slots (no server-side state)
- **Managed connections**: Each maps to a stream slot via `connection_id` + `slot_number`
- **Entitlement gating**: `max_concurrent_conns` from the user's plan tier limits how many slots can have active sessions simultaneously
- **Slot persistence**: Slots survive session stop/start — the label and number persist even when no session is active
