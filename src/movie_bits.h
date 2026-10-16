#pragma once
// Exact guest-state fast path for the movie player's bit-buffer consumer.
// False leaves all state untouched, so the generated function can run instead.
bool movie_bits_advance_fast();
