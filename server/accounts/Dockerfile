FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r requirements.txt && useradd --uid 10001 --create-home service
COPY . /app/server/accounts
USER 10001
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
EXPOSE 7385
CMD ["python", "-m", "uvicorn", "server.accounts.app:create_app", "--factory", "--host", "0.0.0.0", "--port", "7385", "--no-access-log", "--no-proxy-headers"]
