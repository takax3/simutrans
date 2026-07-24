# Simutrans Observer REST API

`openapi.yaml` is the source of truth. `openapi.json` and
`network/rest_api_spec_generated.h` are generated from it and are committed so
the REST API has no runtime dependency on documentation files.

After changing the YAML document, regenerate the derived files from the
repository root:

```sh
python documentation/rest-api/generate_embedded_spec.py
```

The generator requires Python 3 and PyYAML. When Simutrans is started with
`-rest-api-port PORT`, the embedded documents are available at:

- `http://127.0.0.1:PORT/api/v1/openapi.yaml`
- `http://127.0.0.1:PORT/api/v1/openapi.json`

The server accepts read-only browser access from any origin and handles CORS
preflight requests. Swagger UI running on another localhost port can load either
OpenAPI URL and use its `Try it out` actions. Cookies and other credentialed
CORS requests are not supported.

`GET /api/v1/map-info` returns the current map dimensions, date, object counts,
server and pakset compatibility metadata, and the raw and divided OTRP clock
values used by timetable diagrams. It intentionally excludes the minimap image,
save filename, server email address, and client network addresses.

`GET /api/v1/time` returns the same clock object without the other map metadata
for clients that poll time frequently. Map compatibility information also
contains the human-readable OTRP version.

World-monitor clients can use `companies`, `stops`, `lines`, and `convoys` as
structured data sources. Company records include the internal account balance;
stop records separate the owning company from all companies effectively allowed
to stop there, and expose passenger totals and previous-month throughput. Per-stop
passenger destinations and ordered line schedules are available from their
resource-specific paths. All resource IDs must be refreshed when `world_epoch`
changes.
