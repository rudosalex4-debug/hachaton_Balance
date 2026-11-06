FROM python:3.12-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends libseccomp2 && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home runner && mkdir -p /jobs && chown 10001:10001 /jobs
WORKDIR /app
COPY python-worker/runner /app/runner
COPY python-worker/procedures /app/procedures
COPY python-worker/checks /app/checks
USER 10001:10001
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
CMD ["python", "-I", "-B", "/app/runner/daemon.py"]
