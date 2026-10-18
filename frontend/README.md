# HiLily DAISY Book Search Frontend

## Run locally

```bash
cd frontend
npm install
npm run dev
```

The Vite development server runs at `http://localhost:5173`.

Set the backend used by the development proxy when necessary:

```bash
set VITE_API_TARGET=http://4.197.40.220:8000
npm run dev
```

PowerShell:

```powershell
$env:VITE_API_TARGET = "http://4.197.40.220:8000"
npm.cmd run dev
```

## Search API

The frontend sends:

```http
GET /api/books/search?q=garden&letter=G
Accept: application/json
```

It accepts a raw book array or an object containing `books`, `results`, or
`items`.

```json
{
  "message": "Three DAISY books found",
  "audio_url": "/audio/search-response.mp3",
  "books": [
    {
      "id": "book-1",
      "title": "The Secret Garden",
      "authors": ["Frances Hodgson Burnett"],
      "year": 1911,
      "audio_url": "/audio/the-secret-garden.mp3",
      "cover_url": "/covers/the-secret-garden.jpg"
    }
  ]
}
```

Supported alternatives include `author`, `published_year`,
`publication_year`, `stream_url`, and `play_url`.

## Voice commands

- `DAISY Book`
- `Search` or `Search for <title>`
- `Play`, `Play 2`, or `Play <title>`
- `Stop`
- Any single letter from `A` through `Z`

Voice recognition uses the browser Web Speech API when available. Search,
letter navigation, and audio controls remain usable without microphone access.
