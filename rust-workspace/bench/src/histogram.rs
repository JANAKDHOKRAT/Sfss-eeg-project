//! Shared histogram bucket definitions used by BOTH backends.

pub const B: usize = 5;

pub const BUCKETS: [(u8, u16); B] = [
    (0,   50),
    (50,  100),
    (100, 150),
    (150, 200),
    (200, 256),
];

pub fn score_to_bucket(score: u8) -> usize {
    let s = score as u16;
    for (i, &(lo, hi)) in BUCKETS.iter().enumerate() {
        if s >= lo as u16 && s < hi { return i; }
    }
    B - 1
}

pub fn score_to_indicators(score: u8) -> [u32; B] {
    let mut ind = [0u32; B];
    ind[score_to_bucket(score)] = 1;
    ind
}

pub fn bucket_label(i: usize) -> String {
    let (lo, hi) = BUCKETS[i];
    format!("[{:3},{:3})", lo, hi)
}