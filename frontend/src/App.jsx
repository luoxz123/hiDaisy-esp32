import { useEffect, useMemo, useRef, useState } from "react";
import {
  BookOpen,
  Headphones,
  LoaderCircle,
  Mic,
  Pause,
  Play,
  Search,
  Square,
  Volume2,
} from "lucide-react";

const LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ".split("");

const DEMO_BOOKS = [
  {
    id: "demo-1",
    title: "The Secret Garden",
    authors: "Frances Hodgson Burnett",
    year: "1911",
    audioUrl: "",
  },
  {
    id: "demo-2",
    title: "A Little Princess",
    authors: "Frances Hodgson Burnett",
    year: "1905",
    audioUrl: "",
  },
  {
    id: "demo-3",
    title: "Anne of Green Gables",
    authors: "L. M. Montgomery",
    year: "1908",
    audioUrl: "",
  },
];

function normalizeBook(book, index) {
  const authors = Array.isArray(book.authors)
    ? book.authors.join(", ")
    : book.authors || book.author || "Unknown author";

  return {
    id: String(book.id || book.isbn || `${book.title || "book"}-${index}`),
    title: book.title || book.name || "Untitled DAISY book",
    authors,
    year: String(book.year || book.published_year || book.publication_year || "Year unavailable"),
    audioUrl:
      book.audio_url ||
      book.audioUrl ||
      book.stream_url ||
      book.play_url ||
      "",
    coverUrl: book.cover_url || book.coverUrl || book.image_url || "",
  };
}

function getResponseAudio(data) {
  return (
    data?.audio_url ||
    data?.audioUrl ||
    data?.audio_response?.url ||
    data?.feedback_audio_url ||
    ""
  );
}

export default function App() {
  const [query, setQuery] = useState("");
  const [selectedLetter, setSelectedLetter] = useState("");
  const [books, setBooks] = useState([]);
  const [loading, setLoading] = useState(false);
  const [status, setStatus] = useState("Ready to search DAISY books.");
  const [error, setError] = useState("");
  const [listening, setListening] = useState(false);
  const [transcript, setTranscript] = useState("");
  const [activeBookId, setActiveBookId] = useState("");
  const [audioSource, setAudioSource] = useState("");
  const [audioLabel, setAudioLabel] = useState("No audio selected");
  const [playing, setPlaying] = useState(false);

  const audioRef = useRef(null);
  const recognitionRef = useRef(null);

  const speechSupported = useMemo(
    () => Boolean(window.SpeechRecognition || window.webkitSpeechRecognition),
    [],
  );

  useEffect(() => {
    return () => recognitionRef.current?.stop();
  }, []);

  const stopAudio = () => {
    if (audioRef.current) {
      audioRef.current.pause();
      audioRef.current.currentTime = 0;
    }
    setPlaying(false);
    setStatus("Audio stopped.");
  };

  const playAudio = async (source, label, bookId = "") => {
    if (!source) {
      setStatus(`No audio is available for ${label}.`);
      return;
    }

    setAudioSource(source);
    setAudioLabel(label);
    setActiveBookId(bookId);
    setStatus(`Loading audio for ${label}.`);

    window.setTimeout(async () => {
      try {
        await audioRef.current?.play();
        setPlaying(true);
        setStatus(`Playing ${label}.`);
      } catch {
        setPlaying(false);
        setStatus("Audio is ready. Press play to begin.");
      }
    }, 0);
  };

  const searchBooks = async (searchTerm = query, letter = selectedLetter) => {
    const trimmedTerm = searchTerm.trim();
    const params = new URLSearchParams();
    if (trimmedTerm) params.set("q", trimmedTerm);
    if (letter) params.set("letter", letter);

    setLoading(true);
    setError("");
    setStatus("Searching the DAISY catalogue.");

    try {
      const response = await fetch(`/api/books/search?${params.toString()}`, {
        headers: { Accept: "application/json" },
      });

      if (!response.ok) {
        throw new Error(`Search request failed with status ${response.status}`);
      }

      const data = await response.json();
      const rawBooks = Array.isArray(data)
        ? data
        : data.books || data.results || data.items || [];
      const normalized = rawBooks.map(normalizeBook);
      const responseAudio = getResponseAudio(data);

      setBooks(normalized);
      setStatus(
        data.message ||
          data.feedback ||
          `${normalized.length} DAISY ${normalized.length === 1 ? "book" : "books"} found.`,
      );

      if (responseAudio) {
        await playAudio(responseAudio, "server response");
      }
    } catch (requestError) {
      setBooks(DEMO_BOOKS);
      setError(requestError.message);
      setStatus("Backend unavailable. Showing sample book records.");
    } finally {
      setLoading(false);
    }
  };

  const selectLetter = (letter) => {
    const nextLetter = selectedLetter === letter ? "" : letter;
    setSelectedLetter(nextLetter);
    searchBooks(query, nextLetter);
  };

  const playBook = (book) => {
    playAudio(book.audioUrl, book.title, book.id);
  };

  const handleVoiceCommand = (spokenText) => {
    const phrase = spokenText.trim();
    const lower = phrase.toLowerCase();
    setTranscript(phrase);

    if (lower === "stop" || lower.includes("stop audio")) {
      stopAudio();
      return;
    }

    if (lower.startsWith("play")) {
      const numberMatch = lower.match(/\b(\d+)\b/);
      const requestedIndex = numberMatch ? Number(numberMatch[1]) - 1 : 0;
      const requestedTitle = lower.replace(/^play\s*/, "").trim();
      const matchingBook =
        books.find((book) => book.title.toLowerCase().includes(requestedTitle)) ||
        books[requestedIndex] ||
        books[0];

      if (matchingBook) playBook(matchingBook);
      else setStatus("Search for a DAISY book before using play.");
      return;
    }

    const letterMatch = lower.match(/^(?:letter\s+)?([a-z])$/);
    if (letterMatch) {
      selectLetter(letterMatch[1].toUpperCase());
      return;
    }

    const cleanedQuery = phrase
      .replace(/\bdaisy book\b/gi, "")
      .replace(/\bsearch(?: for)?\b/gi, "")
      .trim();
    const nextQuery = cleanedQuery || "DAISY Book";
    setQuery(nextQuery);
    searchBooks(nextQuery, selectedLetter);
  };

  const startVoiceInput = () => {
    const Recognition = window.SpeechRecognition || window.webkitSpeechRecognition;
    if (!Recognition) {
      setStatus("Voice recognition is not supported in this browser.");
      return;
    }

    const recognition = new Recognition();
    recognition.lang = "en-NZ";
    recognition.interimResults = false;
    recognition.maxAlternatives = 1;
    recognition.onstart = () => {
      setListening(true);
      setStatus("Listening for a DAISY book command.");
    };
    recognition.onresult = (event) => {
      handleVoiceCommand(event.results[0][0].transcript);
    };
    recognition.onerror = () => {
      setStatus("Voice input could not be captured.");
    };
    recognition.onend = () => setListening(false);
    recognitionRef.current = recognition;
    recognition.start();
  };

  const runKeyword = (keyword) => {
    if (keyword === "Play") {
      handleVoiceCommand("play");
    } else if (keyword === "Stop") {
      stopAudio();
    } else {
      handleVoiceCommand(keyword);
    }
  };

  return (
    <div className="app-shell">
      <header className="topbar">
        <div className="brand">
          <span className="brand-mark" aria-hidden="true">
            <BookOpen size={24} />
          </span>
          <div>
            <strong>HiLily</strong>
            <span>DAISY book search</span>
          </div>
        </div>
        <div className="topbar-state" aria-live="polite">
          <span className={loading ? "state-dot busy" : "state-dot"} />
          {loading ? "Searching" : "Ready"}
        </div>
      </header>

      <main>
        <section className="search-band" aria-labelledby="search-heading">
          <div className="search-copy">
            <p className="eyebrow">Accessible audio catalogue</p>
            <h1 id="search-heading">Find a DAISY book</h1>

            <form
              className="search-form"
              onSubmit={(event) => {
                event.preventDefault();
                searchBooks();
              }}
            >
              <label htmlFor="book-search">Title, author, or keyword</label>
              <div className="search-row">
                <div className="input-wrap">
                  <Search size={21} aria-hidden="true" />
                  <input
                    id="book-search"
                    value={query}
                    onChange={(event) => setQuery(event.target.value)}
                    placeholder="Search DAISY books"
                    autoComplete="off"
                  />
                </div>
                <button className="primary-button" type="submit" disabled={loading}>
                  {loading ? (
                    <LoaderCircle className="spin" size={20} aria-hidden="true" />
                  ) : (
                    <Search size={20} aria-hidden="true" />
                  )}
                  Search
                </button>
                <button
                  className={listening ? "icon-button listening" : "icon-button"}
                  type="button"
                  onClick={startVoiceInput}
                  disabled={!speechSupported || listening}
                  aria-label={listening ? "Listening" : "Start voice command"}
                  title={speechSupported ? "Start voice command" : "Voice recognition unavailable"}
                >
                  <Mic size={22} aria-hidden="true" />
                </button>
              </div>
            </form>

            <div className="command-row" aria-label="Voice command keywords">
              {["DAISY Book", "Search", "Play", "Stop"].map((keyword) => (
                <button
                  key={keyword}
                  type="button"
                  className="command-button"
                  onClick={() => runKeyword(keyword)}
                >
                  {keyword === "Play" ? <Play size={15} /> : null}
                  {keyword === "Stop" ? <Square size={14} /> : null}
                  {keyword}
                </button>
              ))}
            </div>
          </div>

          <figure className="search-art">
            <img
              src="/daisy-audio-books.png"
              alt="An open braille book with headphones and a daisy bookmark"
            />
          </figure>
        </section>

        <section className="alphabet-section" aria-labelledby="alphabet-heading">
          <div className="section-heading">
            <div>
              <p className="eyebrow">Browse catalogue</p>
              <h2 id="alphabet-heading">A–Z</h2>
            </div>
            {selectedLetter ? (
              <button
                type="button"
                className="text-button"
                onClick={() => selectLetter(selectedLetter)}
              >
                Clear {selectedLetter}
              </button>
            ) : null}
          </div>
          <div className="alphabet-grid">
            {LETTERS.map((letter) => (
              <button
                key={letter}
                type="button"
                className={selectedLetter === letter ? "letter active" : "letter"}
                aria-pressed={selectedLetter === letter}
                onClick={() => selectLetter(letter)}
              >
                {letter}
              </button>
            ))}
          </div>
        </section>

        <section className="workspace">
          <div className="results-column">
            <div className="section-heading results-heading">
              <div>
                <p className="eyebrow">Search results</p>
                <h2>{books.length ? `${books.length} books` : "Books"}</h2>
              </div>
              {transcript ? (
                <p className="transcript">
                  Heard: <strong>{transcript}</strong>
                </p>
              ) : null}
            </div>

            <div className="status-line" role="status" aria-live="polite">
              {loading ? <LoaderCircle className="spin" size={18} /> : <Volume2 size={18} />}
              <span>{status}</span>
            </div>

            {error ? (
              <p className="error-message" role="alert">
                {error}
              </p>
            ) : null}

            <div className="book-grid">
              {books.map((book, index) => (
                <article
                  className={activeBookId === book.id ? "book-card active" : "book-card"}
                  key={book.id}
                >
                  <div className="cover" aria-hidden="true">
                    {book.coverUrl ? (
                      <img src={book.coverUrl} alt="" />
                    ) : (
                      <>
                        <span>{book.title.charAt(0).toUpperCase()}</span>
                        <small>DAISY</small>
                      </>
                    )}
                  </div>
                  <div className="book-details">
                    <span className="result-number">Book {index + 1}</span>
                    <h3>{book.title}</h3>
                    <p>{book.authors}</p>
                    <time>{book.year}</time>
                  </div>
                  <button
                    className="card-play"
                    type="button"
                    onClick={() => playBook(book)}
                    disabled={!book.audioUrl}
                    aria-label={`Play ${book.title}`}
                    title={book.audioUrl ? `Play ${book.title}` : "Audio unavailable"}
                  >
                    <Play size={19} fill="currentColor" aria-hidden="true" />
                  </button>
                </article>
              ))}
            </div>

            {!books.length && !loading ? (
              <div className="empty-state">
                <Headphones size={30} aria-hidden="true" />
                <h3>No books selected</h3>
                <p>Enter a title, author, keyword, or choose a letter.</p>
              </div>
            ) : null}
          </div>

          <aside className="player-panel" aria-labelledby="player-heading">
            <div className="player-icon" aria-hidden="true">
              <Headphones size={25} />
            </div>
            <p className="eyebrow">Audio response</p>
            <h2 id="player-heading">{audioLabel}</h2>
            <audio
              ref={audioRef}
              src={audioSource}
              controls
              preload="metadata"
              onPlay={() => setPlaying(true)}
              onPause={() => setPlaying(false)}
              onEnded={() => {
                setPlaying(false);
                setStatus("Audio finished.");
              }}
            />
            <div className="player-actions">
              <button
                className="primary-button player-button"
                type="button"
                disabled={!audioSource}
                onClick={() => {
                  if (playing) audioRef.current?.pause();
                  else audioRef.current?.play();
                }}
              >
                {playing ? <Pause size={20} /> : <Play size={20} />}
                {playing ? "Pause" : "Play"}
              </button>
              <button
                className="secondary-button player-button"
                type="button"
                disabled={!audioSource}
                onClick={stopAudio}
              >
                <Square size={17} fill="currentColor" />
                Stop
              </button>
            </div>
          </aside>
        </section>
      </main>
    </div>
  );
}
