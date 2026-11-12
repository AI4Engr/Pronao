#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

#[tauri::command]
fn add(a: f64, b: f64) -> f64 {
    a + b
}

#[tauri::command]
fn mean(values: Vec<f64>) -> Result<f64, String> {
    if values.is_empty() {
        return Err("invalid_argument: give at least one number".into());
    }
    Ok(values.iter().sum::<f64>() / values.len() as f64)
}

#[tauri::command]
fn greet(name: String) -> String {
    format!("hello, {}", name)
}

#[tauri::command]
fn fail() -> Result<(), String> {
    Err("this command always fails".into())
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![add, mean, greet, fail])
        .run(tauri::generate_context!())
        .expect("error while running the application");
}
