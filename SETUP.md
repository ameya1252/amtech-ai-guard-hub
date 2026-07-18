# AMTECH AI Guard Hub Setup Notes

## Docker Build Container Dependencies

The Luckfox/Rockchip build should run inside the `luckfox-dev` Docker container, not directly on macOS.

For the C notification client, install libcurl development headers in the container:

```sh
docker exec -it luckfox-dev bash -c "apt-get update && apt-get install -y libcurl4-openssl-dev pkg-config"
```

Verify:

```sh
docker exec -it luckfox-dev bash -c "pkg-config --exists libcurl && echo libcurl-ok && find /usr/include -path '*curl/curl.h' | head"
```

This is needed for real, non-simulated builds of `src/notify_client.c`.

Simulation builds can use:

```sh
-DSIMULATE_NETWORK
```

and do not require libcurl headers.

## WhatsApp Backend

The backend lives in `backend/`.

Run in simulated mode:

```sh
cd backend
PORT=8000 SIMULATE_WHATSAPP=1 python3 app.py
```

Test alerts:

```sh
python3 test_alerts.py
```

Railway deployment is prepared with:

```text
backend/Procfile
backend/requirements.txt
```

The backend binds to `0.0.0.0` and reads Railway's dynamic port from `PORT`.

Live simulated Railway backend:

```text
https://amtech-ai-guard-hub-production.up.railway.app
```

Health check:

```sh
curl https://amtech-ai-guard-hub-production.up.railway.app/health
```

Real WhatsApp Cloud API mode will require Meta credentials and an approved utility template.

## Backend Database

The backend reads its database connection from:

```sh
DATABASE_URL
```

On Railway, this should point to the Neon Postgres connection string.

The backend creates these initial tables automatically at startup:

- `shops`
- `devices`
- `alerts`

For local testing without Neon:

```sh
DATABASE_URL=sqlite:////tmp/amtech_alerts.db PORT=8000 SIMULATE_WHATSAPP=1 python3 backend/app.py
```

## Notification Client URL

The C notification client defaults to the live Railway alert endpoint:

```text
https://amtech-ai-guard-hub-production.up.railway.app/alert
```

Override at runtime:

```sh
export AMTECH_BACKEND_ALERT_URL="http://127.0.0.1:8000/alert"
```

or at compile time:

```sh
-DNOTIFY_BACKEND_URL=\"https://your-railway-app.up.railway.app/alert\"
```

HTTPS is handled by libcurl. No extra SSL code is required as long as the target libcurl build has TLS support, which `libcurl4-openssl-dev` provides in the Docker setup.

The live Railway backend currently has WhatsApp simulation enabled. It accepts real HTTPS alert requests and returns `simulated:true`, but it does not send real Meta WhatsApp messages yet.
