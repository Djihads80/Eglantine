// SPDX-FileCopyrightText: Copyright 2026 fab918
// SPDX-License-Identifier: MIT

use nxformat_packer::{conversion::convert_inventory, native::{inventory, InventoryOptions, synthetic::{dnsp, title_entries, TitleSpec}}, IntegrityPolicy, WriterOptions};
use std::{fs, path::PathBuf};
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let out = PathBuf::from(std::env::args().nth(1).unwrap());
    fs::create_dir_all(&out)?;
    let app = 0x0100123456780000;
    let base = TitleSpec::application(app, 0, 0x11);
    let mut internal = TitleSpec::patch(app, 1, 0x22);
    internal.program.as_mut().unwrap().bktr = false;
    let mut external = TitleSpec::patch(app, 2, 0x33);
    external.program.as_mut().unwrap().bktr = false;
    let dlc = TitleSpec::add_on(app, app + 0x1002, 1, 0x44);
    let base_path = out.join("native_base_source.nsp");
    let internal_path = out.join("native_internal_update.nsp");
    for (path, spec) in [(&base_path, &base), (&internal_path, &internal), (&out.join("external_update.nsp"), &external), (&out.join("external_dlc.nsp"), &dlc)] {
        fs::write(path, dnsp(&title_entries(spec)))?;
    }
    let opts = InventoryOptions::default();
    let inv = inventory(&[base_path.clone()], &opts)?;
    convert_inventory(&inv, &out.join("native_external_base.nx"), &opts, IntegrityPolicy::Strict, &WriterOptions::default())?;
    let inv = inventory(&[base_path.clone(), internal_path.clone()], &opts)?;
    convert_inventory(&inv, &out.join("native_embedded_update.nx"), &opts, IntegrityPolicy::Strict, &WriterOptions::default())?;
    fs::remove_file(base_path)?;
    fs::remove_file(internal_path)?;
    Ok(())
}
