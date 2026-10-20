FROM node:20-alpine

WORKDIR /app

COPY package.json package-lock.json* ./
RUN npm ci --omit=dev

COPY server.js ./server.js
COPY public ./public
COPY esp32-firmware ./esp32-firmware
COPY README.md MANUS_DEPLOY.md .env.example ./

ENV NODE_ENV=production
EXPOSE 3000

CMD ["npm", "start"]
