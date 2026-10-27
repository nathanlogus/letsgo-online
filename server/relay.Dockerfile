# The co-op relay (server/coop_relay.py: standard library only). Built by server/accounts/compose.yml.
FROM python:3.12-slim
WORKDIR /app
RUN useradd --uid 10002 --create-home relay
COPY coop_relay.py /app/coop_relay.py
USER 10002
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 COOP_RELAY_HOST=0.0.0.0 COOP_RELAY_HEALTH_HOST=0.0.0.0
EXPOSE 7381
HEALTHCHECK --interval=30s --timeout=3s CMD ["python", "-c", "import urllib.request as u; u.urlopen('http://127.0.0.1:7382/health', timeout=2)"]
CMD ["python", "coop_relay.py"]
