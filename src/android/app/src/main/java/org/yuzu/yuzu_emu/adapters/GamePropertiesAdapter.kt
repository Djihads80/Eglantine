// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.adapters

import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.core.content.res.ResourcesCompat
import androidx.lifecycle.LifecycleOwner
import com.google.android.material.button.MaterialButton
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.CardConsoleInstallableBinding
import org.yuzu.yuzu_emu.databinding.CardConsolePropertyBinding
import org.yuzu.yuzu_emu.model.GameProperty
import org.yuzu.yuzu_emu.model.InstallableProperty
import org.yuzu.yuzu_emu.model.SubmenuProperty
import org.yuzu.yuzu_emu.utils.ViewUtils.marquee
import org.yuzu.yuzu_emu.utils.ViewUtils.setVisible
import org.yuzu.yuzu_emu.utils.collect
import org.yuzu.yuzu_emu.viewholder.AbstractViewHolder

class GamePropertiesAdapter(
    private val viewLifecycle: LifecycleOwner,
    private var properties: List<GameProperty>
) : AbstractListAdapter<GameProperty, AbstractViewHolder<GameProperty>>(properties) {
    override fun onCreateViewHolder(
        parent: ViewGroup,
        viewType: Int
    ): AbstractViewHolder<GameProperty> {
        val inflater = LayoutInflater.from(parent.context)
        return when (viewType) {
            PropertyType.Submenu.ordinal -> {
                SubmenuPropertyViewHolder(
                    CardConsolePropertyBinding.inflate(
                        inflater,
                        parent,
                        false
                    )
                )
            }

            else -> InstallablePropertyViewHolder(
                CardConsoleInstallableBinding.inflate(
                    inflater,
                    parent,
                    false
                )
            )
        }
    }

    override fun getItemViewType(position: Int): Int {
        return when (properties[position]) {
            is SubmenuProperty -> PropertyType.Submenu.ordinal
            else -> PropertyType.Installable.ordinal
        }
    }

    inner class SubmenuPropertyViewHolder(val binding: CardConsolePropertyBinding) :
        AbstractViewHolder<GameProperty>(binding) {
        override fun bind(model: GameProperty) {
            val submenuProperty = model as SubmenuProperty

            binding.root.setOnClickListener {
                submenuProperty.action.invoke()
            }

            binding.title.setText(submenuProperty.titleId)
            binding.title.setTextColor(
                ResourcesCompat.getColorStateList(
                    binding.title.resources,
                    if (submenuProperty.isDestructive) {
                        R.color.console_on_surface_danger
                    } else {
                        R.color.console_on_surface_primary
                    },
                    binding.title.context.theme
                )
            )
            binding.description.setText(submenuProperty.descriptionId)
            binding.icon.setImageDrawable(
                ResourcesCompat.getDrawable(
                    binding.icon.context.resources,
                    submenuProperty.iconId,
                    binding.icon.context.theme
                )
            )

            binding.details.marquee()
            // Empty values hide the chip (e.g. a value that is still being computed).
            if (submenuProperty.details != null) {
                val text = submenuProperty.details.invoke()
                binding.details.text = text
                binding.details.setVisible(text.isNotEmpty())
            } else if (submenuProperty.detailsFlow != null) {
                binding.details.setVisible(true)
                submenuProperty.detailsFlow.collect(viewLifecycle) {
                    binding.details.text = it
                    binding.details.setVisible(it.isNotEmpty())
                }
            } else {
                binding.details.setVisible(false)
            }


            val hasVisibleActions = submenuProperty.secondaryActions?.any { it.isShown } == true
            binding.layoutSecondaryActions.removeAllViews()
            binding.dividerSecondaryActions.setVisible(false)

            if (hasVisibleActions) {
                binding.layoutSecondaryActions.setVisible(true)

                val visibleActions = submenuProperty.secondaryActions!!.filter { it.isShown }
                val inflater = LayoutInflater.from(binding.root.context)
                visibleActions.forEachIndexed { index, secondaryAction ->
                    val button = inflater.inflate(
                        R.layout.item_console_secondary_action,
                        binding.layoutSecondaryActions,
                        false
                    ) as MaterialButton
                    button.setIconResource(secondaryAction.iconId)
                    button.text = ""
                    button.contentDescription = binding.root.context
                        .getString(secondaryAction.descriptionId)
                    button.tooltipText = binding.root.context
                        .getString(secondaryAction.descriptionId)
                    if (index == visibleActions.lastIndex) {
                        (button.layoutParams as ViewGroup.MarginLayoutParams).marginEnd = 0
                    }
                    button.setOnClickListener { secondaryAction.action.invoke() }
                    binding.layoutSecondaryActions.addView(button)
                }
            } else {
                binding.layoutSecondaryActions.setVisible(false)
            }
        }
    }

    inner class InstallablePropertyViewHolder(val binding: CardConsoleInstallableBinding) :
        AbstractViewHolder<GameProperty>(binding) {
        override fun bind(model: GameProperty) {
            val installableProperty = model as InstallableProperty

            binding.title.setText(installableProperty.titleId)
            binding.description.setText(installableProperty.descriptionId)
            binding.icon.setImageDrawable(
                ResourcesCompat.getDrawable(
                    binding.icon.context.resources,
                    installableProperty.iconId,
                    binding.icon.context.theme
                )
            )

            if (installableProperty.detailsFlow != null) {
                binding.details.setVisible(true)
                installableProperty.detailsFlow.collect(viewLifecycle) {
                    binding.details.text = it
                    binding.details.setVisible(it.isNotEmpty())
                }
            } else {
                binding.details.setVisible(false)
            }

            binding.buttonInstall.setVisible(installableProperty.install != null)
            binding.buttonInstall.setOnClickListener { installableProperty.install?.invoke() }
            binding.buttonExport.setVisible(installableProperty.export != null)
            binding.buttonExport.setOnClickListener { installableProperty.export?.invoke() }
        }
    }

    enum class PropertyType {
        Submenu,
        Installable
    }
}
