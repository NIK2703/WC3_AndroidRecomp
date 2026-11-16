package wc3.recomp.launch

import android.content.Context
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.Filter
import android.widget.Filterable
import android.widget.TextView
import java.util.Locale
import wc3.recomp.R

/**
 * Suggestion list for the DXVK key and value fields.
 *
 * The filter always matches against the complete list, so deleting characters
 * widens the result again, and only the beginning of the typed text is
 * compared.  Every field owns its list: a shared adapter would let one
 * field's filter narrow the list that another field still has to search.
 */
class DxvkSuggestionAdapter(
    context: Context,
    items: List<String>
) : BaseAdapter(), Filterable {

    private val inflater = LayoutInflater.from(context)

    /* The filter runs on a worker thread while the key field may replace the
     * suggestions on the UI thread. */
    @Volatile
    private var source: List<String> = items
    private var shown: List<String> = items

    private val filter = object : Filter() {
        override fun performFiltering(constraint: CharSequence?): FilterResults {
            val prefix = constraint?.toString()?.trim()?.lowercase(Locale.getDefault()).orEmpty()
            val matched = if (prefix.isEmpty()) {
                source
            } else {
                source.filter { it.lowercase(Locale.getDefault()).startsWith(prefix) }
            }
            return FilterResults().apply {
                values = matched
                count = matched.size
            }
        }

        override fun publishResults(constraint: CharSequence?, results: FilterResults?) {
            shown = (results?.values as? List<String>) ?: emptyList()
            notifyDataSetChanged()
        }
    }

    /** Replaces the suggestions, for example when the key of a row changes. */
    fun setItems(items: List<String>) {
        if (source == items) return
        source = items
        shown = items
        notifyDataSetChanged()
    }

    override fun getCount(): Int = shown.size

    override fun getItem(position: Int): String = shown[position]

    override fun getItemId(position: Int): Long = position.toLong()

    override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
        val view = convertView
            ?: inflater.inflate(R.layout.item_resolution_dropdown, parent, false)
        (view as TextView).text = shown[position]
        return view
    }

    override fun getFilter(): Filter = filter
}
