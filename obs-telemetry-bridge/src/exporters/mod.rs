use crate::model::TelemetryFrame;
use opentelemetry::{global, metrics::Histogram, metrics::Meter, metrics::MeterProvider as _, KeyValue};
use opentelemetry_otlp::WithExportConfig;
use opentelemetry_sdk::metrics::reader::{DefaultAggregationSelector, DefaultTemporalitySelector};
use opentelemetry_sdk::metrics::{MeterProvider, PeriodicReader};
use std::{collections::HashMap, time::Duration};

type AnyError = Box<dyn std::error::Error + Send + Sync + 'static>;

pub struct GrafanaExporter {
    meter: Meter,
    health: Histogram<f64>,
    cpu: Histogram<f64>,
    mem: Histogram<f64>,
    gpu: Histogram<f64>,
    upload: Histogram<f64>,
    download: Histogram<f64>,
    latency: Histogram<f64>,
    out_bitrate: Histogram<f64>,
    out_drop: Histogram<f64>,
    out_fps: Histogram<f64>,
    out_lag: Histogram<f64>,
}

impl GrafanaExporter {
    pub fn new(
        endpoint: &str,
        auth_header: &str,
        auth_value: Option<String>,
        interval_ms: u64,
    ) -> Result<Self, AnyError> {
        let mut headers = HashMap::new();
        if let Some(value) = auth_value {
            headers.insert(auth_header.to_string(), value);
        }

        let exporter = opentelemetry_otlp::new_exporter()
            .http()
            .with_endpoint(endpoint)
            .with_headers(headers)
            .build_metrics_exporter(
                Box::new(DefaultAggregationSelector::new()),
                Box::new(DefaultTemporalitySelector::new()),
            )?;

        let reader = PeriodicReader::builder(exporter, opentelemetry_sdk::runtime::Tokio)
            .with_interval(Duration::from_millis(interval_ms))
            .build();

        let provider = MeterProvider::builder().with_reader(reader).build();
        let meter = provider.meter("telemy");
        global::set_meter_provider(provider);

        let health = meter.f64_histogram("telemy.health").init();
        let cpu = meter.f64_histogram("telemy.system.cpu_percent").init();
        let mem = meter.f64_histogram("telemy.system.mem_percent").init();
        let gpu = meter.f64_histogram("telemy.system.gpu_percent").init();
        let upload = meter.f64_histogram("telemy.network.upload_mbps").init();
        let download = meter.f64_histogram("telemy.network.download_mbps").init();
        let latency = meter.f64_histogram("telemy.network.latency_ms").init();
        let out_bitrate = meter.f64_histogram("telemy.output.bitrate_kbps").init();
        let out_drop = meter.f64_histogram("telemy.output.drop_pct").init();
        let out_fps = meter.f64_histogram("telemy.output.fps").init();
        let out_lag = meter.f64_histogram("telemy.output.encoding_lag_ms").init();

        Ok(Self {
            meter,
            health,
            cpu,
            mem,
            gpu,
            upload,
            download,
            latency,
            out_bitrate,
            out_drop,
            out_fps,
            out_lag,
        })
    }

    pub fn record(&self, frame: &TelemetryFrame) {
        self.health.record(frame.health as f64, &[]);
        self.cpu.record(frame.system.cpu_percent as f64, &[]);
        self.mem.record(frame.system.mem_percent as f64, &[]);
        self.gpu
            .record(frame.system.gpu_percent.unwrap_or(0.0) as f64, &[]);
        self.upload.record(frame.network.upload_mbps as f64, &[]);
        self.download.record(frame.network.download_mbps as f64, &[]);
        self.latency.record(frame.network.latency_ms as f64, &[]);

        for out in &frame.streams {
            let labels = [KeyValue::new("output", out.name.clone())];
            self.out_bitrate.record(out.bitrate_kbps as f64, &labels);
            self.out_drop.record(out.drop_pct as f64, &labels);
            self.out_fps.record(out.fps as f64, &labels);
            self.out_lag.record(out.encoding_lag_ms as f64, &labels);
        }
    }
}
