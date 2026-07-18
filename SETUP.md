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
SIMULATE_WHATSAPP=1 python3 app.py
```

Test alerts:

```sh
python3 test_alerts.py
```

Real WhatsApp Cloud API mode will require Meta credentials and an approved utility template.
