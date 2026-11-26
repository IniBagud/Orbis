# Orbis

**Program Utility konversi file untuk Windows dengan antarmuka radial menu.**

Orbis merupakan program shortcut untuk melakukan konversi file. Pilih file target di File Explorer, tekan **Shift + Middle Click**, arahkan kursor ke format tujuan, dan klik kiri untuk mengonversi.

### Cara Kerja & Fitur Utama

- **Tampilan:** Otomatis menampilkan 4 opsi format paling relevan berdasarkan ekstensi file yang dipilih (misal: `.mp4` langsung memunculkan `MP3`, `MKV`, `GIF`, `Compress`), ditambah opsi **More** untuk mengakses kategori lengkap (_Video, Audio, Gambar, Dokumen_).
- **Program arsitektur:** Menggunakan _Windows Event Hook_.`OrbisWatcher` hanya membangunkan aplikasi utama(`OrbisConverter`) saat File Explorer aktif, dan otomatis menutup aplikasi utama secara penuh setelah 2 menit tidak digunakan.
- **UI:** Menggunakan UI hitam minimalist untuk memaksimalkan performa.
- **Konversi:** Menggunakan menggunakan ffmpeg, magick, dan pandoc sebagai inti dari program.

**Download program exe dari ffmpeg, magick, dan pandoc ke build/bin/**
