use std::collections::VecDeque;

#[allow(dead_code)]
pub struct RollingWindow<T> {
    capacity: usize,
    values: VecDeque<T>,
}

impl<T: Clone> RollingWindow<T> {
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity,
            values: VecDeque::with_capacity(capacity),
        }
    }

    pub fn push(&mut self, value: T) {
        if self.values.len() == self.capacity {
            self.values.pop_front();
        }
        self.values.push_back(value);
    }

    pub fn values(&self) -> Vec<T> {
        self.values.iter().cloned().collect()
    }
}
