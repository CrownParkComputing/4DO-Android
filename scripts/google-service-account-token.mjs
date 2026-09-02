#!/usr/bin/env node

import { appendFileSync } from 'node:fs';
import { createSign } from 'node:crypto';

const credentialsText = process.env.PLAY_SERVICE_ACCOUNT_JSON;
const outputPath = process.env.GITHUB_OUTPUT;

if (!credentialsText || !outputPath) {
  throw new Error('PLAY_SERVICE_ACCOUNT_JSON and GITHUB_OUTPUT are required');
}

const credentials = JSON.parse(credentialsText);
if (credentials.type !== 'service_account' || !credentials.client_email || !credentials.private_key) {
  throw new Error('PLAY_SERVICE_ACCOUNT_JSON is not a valid service-account key');
}

const encode = (value) => Buffer.from(value).toString('base64url');
const now = Math.floor(Date.now() / 1000);
const unsignedToken = [
  encode(JSON.stringify({ alg: 'RS256', typ: 'JWT' })),
  encode(JSON.stringify({
    iss: credentials.client_email,
    scope: 'https://www.googleapis.com/auth/androidpublisher',
    aud: credentials.token_uri || 'https://oauth2.googleapis.com/token',
    iat: now,
    exp: now + 3600,
  })),
].join('.');

const signer = createSign('RSA-SHA256');
signer.update(unsignedToken);
signer.end();
const assertion = `${unsignedToken}.${signer.sign(credentials.private_key, 'base64url')}`;

const tokenUri = credentials.token_uri || 'https://oauth2.googleapis.com/token';
const response = await fetch(tokenUri, {
  method: 'POST',
  headers: { 'content-type': 'application/x-www-form-urlencoded' },
  body: new URLSearchParams({
    grant_type: 'urn:ietf:params:oauth:grant-type:jwt-bearer',
    assertion,
  }),
});

if (!response.ok) {
  throw new Error(`OAuth token request failed (${response.status}): ${await response.text()}`);
}

const token = (await response.json()).access_token;
if (!token) {
  throw new Error('OAuth response did not include an access token');
}

console.log(`::add-mask::${token}`);
appendFileSync(outputPath, `access_token=${token}\n`, { mode: 0o600 });
