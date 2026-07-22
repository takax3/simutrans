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
